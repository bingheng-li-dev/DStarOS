/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "sched.h"
#include "fpu.h"
#include "trap.h"
#include "proc.h"
#include "rbtree.h"
#include "stringops.h"
#include "dassert.h"
#include "tick.h"
#include "cpu.h"
#include "ktime.h"

/* 按 proc_wake_time_ns 升序排列的定时睡眠链表，sched_sleep_ns()/sched_check_timers() 共用。
 * 静态用 LIST_HEAD_INIT 而不是留给 sched_init() 运行时 INIT_LIST_HEAD：tick_int_handler()
 * 里 sched_check_timers() 是无条件调用的，若 timer 中断在 sched_init() 跑到这一行之前先触发
 * （200 Hz tick 下，hart0 的 fs_init() 挂载/格式化 ramdisk 耗时可能超过一个 tick 周期），
 * BSS 零初始化的 next/prev 都是 NULL，list_for_each_safe 会立刻解引用 NULL 崩溃——
 * 这正是 sched_init() 里 RT ready_lists 那条注释警告过的同一类"清零链表头"陷阱，
 * 只是这次的触发窗口在 sched_init() 自身执行完成之前。静态初始化后 sleeping_tasks
 * 从链接时刻起就是合法的空链表，不再依赖任何运行时初始化顺序。 */
static struct list_head sleeping_tasks = LIST_HEAD_INIT(sleeping_tasks);
static osslock_t sleeping_tasks_lock;

static void   fair_enqueue(pcb_t *p);
static void   fair_dequeue(pcb_t *p);
static pcb_t *fair_pick_next(void);
static void   fair_task_tick(pcb_t *curr);
static void   fair_check_preempt(pcb_t *curr, pcb_t *p);
static void   rt_enqueue(pcb_t *p);
static void   rt_dequeue(pcb_t *p);
static pcb_t *rt_pick_next(void);
static void   rt_task_tick(pcb_t *curr);
static void   rt_check_preempt(pcb_t *curr, pcb_t *p);
static void   idle_enqueue(pcb_t *p);
static void   idle_dequeue(pcb_t *p);
static pcb_t *idle_pick_next(void);
static void   idle_task_tick(pcb_t *curr);
static void   idle_check_preempt(pcb_t *curr, pcb_t *p);
static void   kick_idle_harts(void);

/* 类链：rt -> fair(cfs) -> idle */
const sched_class_t fair_sched_class = {
    .next = &idle_sched_class,
    .enqueue_task = fair_enqueue,
    .dequeue_task = fair_dequeue,
    .pick_next_task = fair_pick_next,
    .task_tick = fair_task_tick,
    .check_preempt = fair_check_preempt,
};
const sched_class_t rt_sched_class = {
    .next = &fair_sched_class,
    .enqueue_task = rt_enqueue,
    .dequeue_task = rt_dequeue,
    .pick_next_task = rt_pick_next,
    .task_tick = rt_task_tick,
    .check_preempt = rt_check_preempt,
};
/* 类链链尾：pick_next 永远返回本 CPU 的 idle_proc，其余方法皆为空操作 */
const sched_class_t idle_sched_class = {
    .next = NULL,
    .enqueue_task = idle_enqueue,
    .dequeue_task = idle_dequeue,
    .pick_next_task = idle_pick_next,
    .task_tick = idle_task_tick,
    .check_preempt = idle_check_preempt,
};

static rq_t run_queue;

static const int nice_to_weight[40] = {
 /* -20 */ 88761, 71755, 56483, 46273, 36291,
 /* -15 */ 29154, 23254, 18705, 14949, 11916,
 /* -10 */  9548,  7620,  6100,  4904,  3906,
 /*  -5 */  3121,  2501,  1991,  1586,  1277,
 /*   0 */  1024,   820,   655,   526,   423,
 /*   5 */   335,   272,   215,   172,   137,
 /*  10 */   110,    87,    70,    56,    45,
 /*  15 */    36,    29,    23,    18,    15,
};

static uint32_t fair_nice_to_weight(int nice)
{
    return nice_to_weight[nice + 20];
}

/**
 * @brief 更新cfs算法的进程的vruntime和min_vruntime
 * @attention 该函数由sched_schedule调用，需要持有run_queue.lock spinlock
 * @note 不需要担心溢出/翻转/回绕，进程需要连续运行几百年才会出现
 * @details min_vruntime是为了防止一个刚创建或睡眠太久的进程，其拥有一个极小的vruntime，
 * 因此会报复性地长时间占用CPU。min_vruntime单调递增，强制给定一个min_vruntime最小值
 */
static void fair_update_curr(pcb_t *curr)
{
    /* vruntime只有cfs算法用到 */
    if (curr->proc_sched_class != &fair_sched_class)
    {
        return;
    }

    uint64_t now = sched_now();
    uint64_t delta = now - curr->proc_exec_start; /* proc_exec_start在sched_set_current被更新了 */
    curr->proc_exec_start = now;

    /* 累计原始（未加权）执行时间，只增不清零；配合换入时刻拍的快照
     * proc_sum_exec_runtime_prev（见 sched_set_current），两者之差就是
     * "这次换上 CPU 以来跑了多久"，供 fair_task_tick 判断是否跑满 SCHED_MIN_GRANULARITY */
    curr->proc_sum_exec_runtime += delta;

    /* Δvruntime = delta * nice0 / proc_weight，**除不尽的余数必须留到下次**。
     *
     * sched_now() 的单位是 time CSR 的一格（QEMU 上 100 ns），而 delta 是"这次在 CPU 上
     * 待了多久"。频繁主动让出的任务每次只待几百纳秒，于是 delta * 1024 常常小于
     * proc_weight——整数除法直接得 0。这对高权重任务是灾难性的：nice-10 的权重 9548，
     * 只要 delta < 9548/1024 ≈ 9.3 格（932 ns）它的 vruntime 就**一格都不涨**，而同样
     * 短的一次运行给 nice+10（权重 110）涨 9。结果是高权重任务的 vruntime 永远停在原地、
     * 独占 CPU，低权重任务跑一次就被顶到队尾再也回不来——**完全丧失加权公平，退化成
     * 高权重独占**。实测（sched_test 的公平性用例，预算 4000）：低权重两个 worker 合计
     * 只拿到 2 次，而理论份额是 45 次，且把预算从 400 加到 4000 这个数一点不变——
     * "份额与预算无关"正是被饿死而不是分得少的特征。
     *
     * 把余数累加回下一次，换算就是精确的：无论切片多短，时间都不会凭空丢掉。
     * （Linux 走的是另一条路——预计算 inv_weight 做定点乘法移位，精度同样够，
     * 但要多一张表；这里任务数少，留余数更直白。） */
    uint64_t numerator = delta * SCHED_NICE_0_WEIGHT + curr->proc_vruntime_rem;
    curr->proc_vruntime     += numerator / curr->proc_weight;
    curr->proc_vruntime_rem  = (uint32_t)(numerator % curr->proc_weight);

    /* 比较rbtree最左节点进程vruntime和当前进程vruntime，
     * 选择更小的那个与旧的min_vruntime比较，择其大者 */
    uint64_t min_vruntime = curr->proc_vruntime;
    /* 判空只能看树本身，不能看 nr_running——测一个、用另一个，两者一旦不同步
     * 就会把 rb_first() 的 NULL 当成有效节点。fair_pick_next 一直是这么写的。 */
    struct rb_node *first = rb_first(&run_queue.cfs.tasks);
    if (first != NULL)
    {
        pcb_t *leftmost = rb_entry(first, pcb_t, proc_rbtree_node);
        if (leftmost->proc_vruntime < curr->proc_vruntime)
        {
            min_vruntime = leftmost->proc_vruntime;
        }
    }
    if (min_vruntime > run_queue.cfs.min_vruntime)
    {
        run_queue.cfs.min_vruntime = min_vruntime;
    }
}

static void fair_enqueue(pcb_t *p)
{
    /* 入队钳制：新建任务 proc_vruntime 恒为 0，长期睡眠任务的 proc_vruntime 停在
     * 睡前那一刻，而 min_vruntime 这段时间一直在单调前推——不钳制的话这类任务
     * 会以一个远低于当前最低的 vruntime 插入红黑树最左侧，换入后长时间垄断 CPU
     * （报复性调度）。钳到 min_vruntime 只会把"过低"的抬平，不影响本来就
     * >= min_vruntime 的任务（比如 sched_schedule() 里仍 RUNNING 被重新入队的
     * curr，它的 vruntime 刚被 fair_update_curr 结算过，本就不低）。 */
    if (p->proc_vruntime < run_queue.cfs.min_vruntime)
    {
        p->proc_vruntime = run_queue.cfs.min_vruntime;
    }

    /* 父节点里指向子的指针的地址&rb_right/&rb_left，如果 parent 是 NULL，则是&root->rb_node */
    struct rb_node **link = &run_queue.cfs.tasks.rb_node;
    /* 新节点的父节点，可以为 NULL（根节点） */
    struct rb_node *parent = NULL;

    /* 根据vruntime遍历红黑树，找到要插入的位置（即普通二叉平衡树BST查找） */
    while (*link)
    {
        parent = *link;
        pcb_t *pp = rb_entry(*link, pcb_t, proc_rbtree_node);
        if (p->proc_vruntime < pp->proc_vruntime)
        {
            link = &(*link)->rb_left;
        }
        else
        {
            link = &(*link)->rb_right;
        }
    }
    /* 将要插入节点挂入红黑树 */
    rb_link_node(&p->proc_rbtree_node, parent, link);
    /* 红黑树重新平衡 */
    rb_insert_color(&p->proc_rbtree_node, &run_queue.cfs.tasks);
    run_queue.cfs.nr_running += 1;
}

static void fair_dequeue(pcb_t *p)
{
    rb_erase(&p->proc_rbtree_node, &run_queue.cfs.tasks);
    run_queue.cfs.nr_running -= 1;
}

static pcb_t * fair_pick_next(void)
{
    if (RB_EMPTY_ROOT(&run_queue.cfs.tasks))
    {
        return NULL;
    }
    
    return rb_entry(rb_first(&run_queue.cfs.tasks), pcb_t, proc_rbtree_node);
}

static void fair_task_tick(pcb_t *curr)
{
    fair_update_curr(curr);

    struct rb_node *first = rb_first(&run_queue.cfs.tasks);
    if (first != NULL)
    {
        pcb_t *leftmost = rb_entry(first, pcb_t, proc_rbtree_node);
        if (curr->proc_vruntime >= leftmost->proc_vruntime + SCHED_WAKEUP_GRANULARITY)
        {
            curr->need_resched = true;
            return;
        }
    }

    /* 即便暂时没有明显落后于最左任务，换入以来只要跑满了最小粒度，也该考虑让位 */
    if (curr->proc_sum_exec_runtime - curr->proc_sum_exec_runtime_prev >= SCHED_MIN_GRANULARITY)
    {
        curr->need_resched = true;
    }
}

static void fair_check_preempt(pcb_t *curr, pcb_t *p)
{
    if (curr->proc_vruntime > p->proc_vruntime + SCHED_WAKEUP_GRANULARITY)
    {
        curr->need_resched = true;
    }
}

static void rt_enqueue(pcb_t *p)
{
    list_add_tail(&p->proc_rt_linker, &run_queue.rt.ready_lists[p->proc_rt_priority]);
    run_queue.rt.bitmap |= 1u << p->proc_rt_priority;
    run_queue.rt.nr_running += 1;
}

static void rt_dequeue(pcb_t *p)
{
    list_del(&p->proc_rt_linker);
    if (list_empty(&run_queue.rt.ready_lists[p->proc_rt_priority]))
    {
        run_queue.rt.bitmap &= ~(1u << p->proc_rt_priority);
    }
    run_queue.rt.nr_running -= 1;
}

static uint8_t rt_highest_prio(uint32_t bitmap)
{
    uint8_t b = RT_MAX_PRIO - 1;
    while ((bitmap & (1u << b)) == 0)
    {
        b -= 1;
    }
    return b;
}

static pcb_t * rt_pick_next(void)
{
    if (run_queue.rt.bitmap == 0)
    {
        return NULL;
    }

    uint8_t prio = rt_highest_prio(run_queue.rt.bitmap);
    pcb_t *next = list_entry(run_queue.rt.ready_lists[prio].next, pcb_t, proc_rt_linker);

    return next;
}

static void rt_task_tick(pcb_t *curr)
{
    /* SCHED_FIFO 什么都不做，跑到主动让出/阻塞为止 */
    if (curr->proc_policy != SCHED_RR)
    {
        return;
    }

    uint64_t now = sched_now();
    uint64_t delta = now - curr->proc_exec_start; /* proc_exec_start在sched_set_current被更新了 */
    curr->proc_exec_start = now;
    curr->proc_sum_exec_runtime += delta;
    /* SCHED_RR 判断时间片是否耗尽 */
    if (curr->proc_sum_exec_runtime - curr->proc_sum_exec_runtime_prev >= RT_SCHED_RR_TIMESLICE)
    {
        curr->need_resched = true;
    }
}

static void rt_check_preempt(pcb_t *curr, pcb_t *p)
{
    if (curr->proc_rt_priority < p->proc_rt_priority)
    {
        curr->need_resched = true;
    }
}

/* idle 从不真正入队/出队——它不是"排队等着轮到自己"的任务，是任何类都没有
 * 就绪任务时的兜底，永远处于"可运行"状态，不需要挂进红黑树/链表这类结构 */
static void idle_enqueue(pcb_t *p)
{
    (void)p;
    return;
}

static void idle_dequeue(pcb_t *p)
{
    (void)p;
    return;
}

static pcb_t *idle_pick_next(void)
{
    /* 类链链尾，恒有效：只要走到这一步说明 rt/fair 都没有就绪任务，
     * 由本 CPU 自己的 idle_proc 兜底，从不返回 NULL */
    return cpu_get_current()->idle_proc;
}

static void idle_task_tick(pcb_t *curr)
{
    (void)curr;
    return;
}

static void idle_check_preempt(pcb_t *curr, pcb_t *p)
{
    (void)curr;
    (void)p;
    return;
}

static pcb_t *pick_next_task(void)
{
    for (const sched_class_t *c = &rt_sched_class; c != NULL; c = c->next)
    {
        pcb_t *p = c->pick_next_task();
        if (p != NULL)
        {
            return p;
        }
    }

    return NULL;
}

/* 类链中越靠前优先级越高：返回类在链中的位置，越小越优先 */
static int class_prio(const sched_class_t *c)
{
    int i = 0;
    for (const sched_class_t *x = &rt_sched_class; x != NULL; x = x->next, i++)
    {
        if (x == c)
        {
            return i;
        }
    }

    return 0x7fffffff;
}

/* 每当有新的进程加入就绪队列（fork和wakeup），需要调用一次判断新就绪的p是否需要抢占当前curr */
static void check_preempt_curr(pcb_t *curr, pcb_t *p)
{
    if (p->proc_sched_class != curr->proc_sched_class)
    {
        if (class_prio(p->proc_sched_class) < class_prio(curr->proc_sched_class))
        {
            curr->need_resched = true;   /* p 属更高优先级的类 → 抢占 */
        }
        return;
    }
    /* 同类：委托给该类的 check_preempt（CFS 比 vruntime，RT 比 rt_priority） */
    curr->proc_sched_class->check_preempt(curr, p);
}

void sched_init(void)
{
    spinlock_init(&run_queue.lock);
    run_queue.cfs.tasks = RB_ROOT;
    run_queue.cfs.min_vruntime = 0;
    run_queue.cfs.nr_running = 0;
    /* 每条 RT 就绪链表必须 INIT_LIST_HEAD（next=prev=自身），不能 memset 清零：
     * 清零后的链表头 prev/next 都是 NULL，首次 list_add_tail 会解引用 head->prev(=NULL)
     * 向地址 0 写入而 Store page fault。 */
    for (int i = 0; i < RT_MAX_PRIO; i++)
    {
        INIT_LIST_HEAD(&run_queue.rt.ready_lists[i]);
    }
    run_queue.rt.bitmap = 0;
    run_queue.rt.nr_running = 0;

    INIT_LIST_HEAD(&sleeping_tasks);
    spinlock_init(&sleeping_tasks_lock);
}

void sched_set_current(pcb_t *p)
{
    cpu_get_current()->current_proc = p;
    p->proc_on_cpu = true;
    p->proc_exec_start = sched_now();
    /* 换入快照：往后 fair_task_tick 用 proc_sum_exec_runtime - 这份快照
     * 算出"这次换上 CPU 以来跑了多久"，判断是否已跑满 SCHED_MIN_GRANULARITY */
    p->proc_sum_exec_runtime_prev = p->proc_sum_exec_runtime;
}

void sched_schedule(void)
{
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);

    pcb_t *curr = proc_get_current();
    /* 这把锁由"被换上的那条执行流"接力释放（见 sched_finish_switch），所以 key 要
     * 随任务走：存进自己的 pcb，将来自己被换回来时再取出来用。 */
    curr->proc_rq_key = run_queue_lock_key;

    /* 护栏：本 hart 连续调度了这么多次，tick 却一格没动——时钟中断停了。
     * 2026-08-31 遇到过一次：某个任务把中断关着被换上，此后该 hart 的定时器再没
     * 响过，睡眠任务永远醒不来，表现是整机静默卡死，靠加心跳探针空转两千万圈才
     * 反推出来。在这里当场 panic 才拿得到现场，代价只是每次调度一次比较。
     * 阈值给得很松：公平性用例每个 tick 也才几千次调度，这里留了两个数量级余量。 */
    cpu_t *sched_cpu = cpu_get_current();
    if (sched_cpu->tick == sched_cpu->sched_last_tick)
    {
        sched_cpu->sched_same_tick += 1;
        if (sched_cpu->sched_same_tick > 1000000u)
        {
            panic("hart %d: timer stalled (tick=%ld stuck, sie=%d, pid=%d)",
                  (int)cpu_get_core_id(), sched_cpu->tick,
                  (int)((read_csr(sstatus) & SSTATUS_SIE) != 0), curr->proc_pid);
        }
    }
    else
    {
        sched_cpu->sched_last_tick = sched_cpu->tick;
        sched_cpu->sched_same_tick = 0;
    }
    fair_update_curr(curr);
    if (curr->proc_state == RUNNING && curr->proc_sched_class != &idle_sched_class)
    {
        sched_enqueue(curr);
    }

    pcb_t *next = pick_next_task();
    if (next->proc_sched_class != &idle_sched_class)
    {
        sched_dequeue(next);
    }
    next->need_resched = false;
    sched_set_current(next);

    /* 锁必须一直持有到 switch_to 真正把 curr 的上下文保存完，不能提前释放：
     * curr 在上面已被 sched_enqueue() 放回共享就绪队列，但此刻它的 proc_context
     * 里还是上一次换出时的旧值。若在 switch_to 之前放锁，另一个 hart 可以立即从
     * 队列里挑走 curr，并按这份尚未写入的旧上下文把它"换上"——同一个任务会在两个
     * hart 上用同一个内核栈并发执行，后果是随机的内存/状态损坏。
     *
     * 因此改由"被换上的执行流"在 switch_to 之后释放这把锁：谁被换上，谁负责补上
     * 前一条执行流欠下的那次释放，用的是它自己 pcb 里存的 proc_rq_key。
     * switch_to 是纯汇编、内部不获取任何锁，临界区长度有界，不会死锁。 */
    if (next != curr)
    {
        /* 记下"待放手"的任务：它的 proc_on_cpu 只能等 switch_to 把上下文真正写完
         * 之后才允许清零，而那已经是被换上来的执行流在跑了，所以经 per-hart 的
         * prev_proc 传递给它（见 sched_finish_switch） */
        cpu_get_current()->prev_proc = curr;
        /* 浮点上下文只在**真正发生切换**时存取。这里存、由被换上的那条执行流在
         * sched_finish_switch() 里恢复——next == curr 那条路径两者都不做，否则就是
         * 拿上次换出时的旧值盖掉此刻活着的寄存器。 */
        fpu_save(curr);
        switch_to(&curr->proc_context, &next->proc_context);
        /* curr 这条执行流将来被换回来时从这里继续；此刻持有的是"把它换回来的那条
         * 执行流"acquire 的锁，由下面这次 release 接力放掉 */
    }

    sched_finish_switch();
}

void sched_finish_switch(void)
{
    /* 仍持有 run_queue.lock，且已经跑在换入后的 hart 上：此刻上一个任务的 switch_to
     * 已经把它的 proc_context 完整写盘，可以安全地宣告"它不再占用任何 hart"，
     * 别的 hart 从这一刻起才被允许把它挑走换上。清零必须在放锁之前完成，
     * 才能和 sched_activate() 里的 proc_on_cpu 判断被同一把锁串行化。 */
    cpu_t *cpu = cpu_get_current();
    /* prev_proc 非空 <=> 刚刚真的换过任务，这正是"该不该恢复浮点"的判据：正常切换与
     * fork_out（新任务第一次被换上，它不会从 switch_to 返回）都走这里，而 next == curr
     * 那条路径 prev_proc 是空的，一次都不碰。 */
    bool switched = (cpu->prev_proc != NULL);
    if (cpu->prev_proc != NULL)
    {
        cpu->prev_proc->proc_on_cpu = false;
        cpu->prev_proc = NULL;
    }
    if (switched)
    {
        fpu_restore(cpu->current_proc);
    }

    /* 用**被换上的这个任务自己**存下的 key：它是这条执行流当初 acquire 时记下的
     * "进来之前中断是开是关"。绝不能读 per-CPU 的槽——acquire 在别的执行流、
     * 甚至别的 hart 上发生，per-CPU 槽里的值与这次 release 并不配对。 */
    pcb_t *me = cpu->current_proc;
    spinlock_release(&run_queue.lock, me->proc_rq_key);

}

void sched_enqueue(pcb_t *p)
{
    if (p->proc_on_rq) /* 避免重复入队 */
    {
        return;
    }

    p->proc_sched_class->enqueue_task(p);
    p->proc_on_rq = true;
}

void sched_dequeue(pcb_t *p)
{
    if (!p->proc_on_rq)
    {
        return;
    }

    p->proc_sched_class->dequeue_task(p);
    p->proc_on_rq = false;
}

void sched_activate(pcb_t *p)
{
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);

    /* p 可能正在另一个 hart 上执行（典型场景：它刚把自己标成待睡眠、放掉了外层的
     * 条件锁，但还没走到 sched_schedule 的 switch_to）。这种情况下绝不能入队——
     * 它的 proc_context 还没保存，被别的 hart 挑走就会用陈旧上下文重复执行同一个
     * 任务。唤醒也不会因此丢失：wakeup() 已经把它的状态改回 RUNNING，它自己走到
     * sched_schedule 时会看到 RUNNING 而把自己重新入队。两条路径都在 run_queue.lock
     * 下，被这把锁严格串行化，不存在中间态。 */
    bool enqueued = false;
    if (!p->proc_on_cpu)
    {
        sched_enqueue(p);
        /* p 是刚变为就绪的外部任务，
         * 拿它和当前正在跑的任务比一次，看要不要立刻抢占 */
        check_preempt_curr(proc_get_current(), p);
        enqueued = true;
    }

    spinlock_release(&run_queue.lock, run_queue_lock_key);

    if (enqueued)
    {
        kick_idle_harts();
    }
}

/**
 * @brief 就绪队列新增任务后，主动 IPI 踢醒正在 wfi 空转的其它 hart
 * @details
 *   不踢的话，空转的 hart 要等下一次 tick 中断（约 0.2 秒）才会回到 idle() 循环里
 *   重新查看共享就绪队列，新任务的最坏发现延迟就是一个 tick。
 *
 *   刻意放在 run_queue.lock 之外调用：发 IPI 是一次 SBI ecall，不该把锁持有时间
 *   拉长到一次 firmware 调用上。因此这里读 current_proc/idle_proc 是不加锁的，
 *   但这纯属"建议性"通知，读到过期值不影响正确性——多发一次，对方醒来发现没活干
 *   会重新 wfi 睡回去；漏发一次，对方最迟下一个 tick 也会自己发现。rv64 上对齐的
 *   8 字节指针读写是原子的，不会读到撕裂值。
 *
 * @note 不存在"刚判断完 idle、IPI 却在对方执行 wfi 之前送达"的丢失窗口：RISC-V 的
 *   wfi 只要有中断处于 pending 状态就会立刻返回（不要求 sstatus.SIE 打开），
 *   先到的 IPI 会把 sip.SSIP 置上，对方的 wfi 因此不会睡下去。
 */
static void kick_idle_harts(void)
{
    uint64_t self = cpu_get_core_id();

    for (uint64_t i = 0; i < CORE_NUMBER; i++)
    {
        if (i == self)
        {
            continue;
        }

        cpu_t *cpu = cpu_get_by_index((uint16_t)i);
        if (cpu->current_proc != NULL && cpu->current_proc == cpu->idle_proc)
        {
            cpu_send_ipi(i);
        }
    }
}

void wakeup(pcb_t *proc)
{
    proc->proc_state = RUNNING;
    sched_activate(proc);
}

void sleep(pcb_t *proc, sta_t state)
{
    dassert(proc == proc_get_current());
    proc->proc_state = state;
    sched_schedule();
}

/**
 * @brief 当前任务定时睡眠，到期由 tick 中断唤醒
 * @param[in] ns 要睡眠的纳秒数
 * @details 在 sleeping_tasks_lock 保护下把 proc_state 置 INTERRUPTIBLE
 *   并按 proc_wake_time_ns 升序插入 sleeping_tasks，随后才 sched_schedule()。
 *   状态赋值与入链在同一把锁下完成，是为了不丢唤醒——万一 ns 极小，
 *   插入后、sched_schedule() 真正切换走前就被 sched_check_timers() 抢先
 *   唤醒（proc_state 改回 RUNNING 并入就绪队列），sched_schedule() 发现
 *   当前任务已是 RUNNING，会按"主动让出"处理而不会重复入队，不会丢事件。
 *
 *   返回前**无条件**摘一次链：到期那条路径 sched_check_timers() 已经摘过，
 *   list_empty 判据会挡住第二次；被信号唤醒那条路径这里是唯一的摘链点。
 *   "无条件摘一次 + 幂等的摘链函数"比"分两条路径各摘各的"可靠得多——阶段 7 的
 *   waitq_remove 就是这么收的口。
 * @note 与 tick_delay() 的忙等自旋不同，本函数会真正让出 CPU。
 * @note 到期检查点只有每个 tick 一次，实际睡眠时长向上取整到 tick 边界。
 */
void sched_sleep_ns(uint64_t ns)
{
    pcb_t *curr = proc_get_current();
    uint64_t wake_at = ktime_get_ns() + ns;

    irq_key_t sleeping_tasks_lock_key = spinlock_acquire(&sleeping_tasks_lock);

    curr->proc_wake_time_ns = wake_at;
    curr->proc_state = INTERRUPTIBLE;

    struct list_head *pos;
    list_for_each(pos, &sleeping_tasks)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_timer_linker);
        if (p->proc_wake_time_ns > wake_at)
        {
            break;
        }
    }
    list_add_tail(&curr->proc_timer_linker, pos);

    spinlock_release(&sleeping_tasks_lock, sleeping_tasks_lock_key);

    sched_schedule();

    sched_timer_remove(curr);
}

void sched_sleep_ticks(uint64_t ticks)
{
    sched_sleep_ns(ticks * (NSEC_PER_SEC / TICK_HZ));
}

/**
 * @brief 把任务从 sleeping_tasks 上摘下来，幂等
 * @details 判据是 list_empty(&p->proc_timer_linker)——所以入链之外的每一处
 *   摘链都必须用 list_del_init 而不是 list_del，否则节点摘掉之后 next/prev
 *   还指着链表，list_empty 返回假，本函数会对一个不在链上的节点再 list_del 一次。
 */
void sched_timer_remove(pcb_t *p)
{
    irq_key_t sleeping_tasks_lock_key = spinlock_acquire(&sleeping_tasks_lock);
    if (!list_empty(&p->proc_timer_linker))
    {
        list_del_init(&p->proc_timer_linker);
    }
    spinlock_release(&sleeping_tasks_lock, sleeping_tasks_lock_key);
}

/**
 * @brief 唤醒 sleeping_tasks 中所有已到期的任务
 * @details 由 tick_int_handler() 每次 tick 调用。sleeping_tasks 按
 *   proc_wake_time_ns 升序排列，一旦遇到未到期的节点即可停止扫描。
 * @note wakeup() 内部会取 run_queue.lock；本函数持有的 sleeping_tasks_lock
 *   在所有路径上都只会是外层锁（sched_sleep_ns() 插入时早已释放它，
 *   之后才单独去拿 run_queue.lock），不会与 run_queue.lock 形成加锁顺序反转。
 */
void sched_check_timers(void)
{
    uint64_t now = ktime_get_ns();
    struct list_head *pos, *tmp;

    irq_key_t sleeping_tasks_lock_key = spinlock_acquire(&sleeping_tasks_lock);
    list_for_each_safe(pos, tmp, &sleeping_tasks)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_timer_linker);
        if (p->proc_wake_time_ns > now)
        {
            break;
        }
        list_del_init(&p->proc_timer_linker);
        wakeup(p);
    }
    spinlock_release(&sleeping_tasks_lock, sleeping_tasks_lock_key);
}

void sched_task_tick(void)
{
    pcb_t *curr = proc_get_current();
    /* 本 hart 尚未执行到 proc_init()（current_proc 还没被 sched_set_current() 设置）
     * 时，timer 中断可能已经先触发——200 Hz tick 下，hart0 的 fs_init() 挂载/格式化
     * ramdisk 耗时可能超过一个 tick 周期，使得 trap_init() 打开的定时器中断在
     * proc_init() 之前就打进来。此时无当前任务可结算，直接跳过。 */
    if (curr == NULL)
    {
        return;
    }

    /* task_tick 会读就绪队列（CFS 要取最左节点算抢占），必须与另一个 hart 上的
     * sched_enqueue/sched_dequeue 互斥。不加锁时曾稳定复现：本 hart 读到
     * cfs.nr_running > 0，随即另一个 hart 在锁内做完 rb_erase 把树清空，
     * 这里的 rb_first() 就返回 NULL，rb_entry(NULL, ...) 得到一个负地址，
     * 内核态访问它直接 segfault（va=0xffffffffffffffe0）。
     * 锁序：本函数由 tick_int_handler 在释放 tick_lock 之后调用，不嵌套在 tick_lock 内。 */
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);
    curr->proc_sched_class->task_tick(curr);
    spinlock_release(&run_queue.lock, run_queue_lock_key);
}

void sched_set_nice(pcb_t *p, int nice)
{
    if (nice < SCHED_NICE_MIN)
    {
        nice = SCHED_NICE_MIN;
    }
    else if (nice > SCHED_NICE_MAX)
    {
        nice = SCHED_NICE_MAX;
    }

    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);

    /* 换权重之前，先把**已经跑过、但还没结算进 vruntime 的那段时间**按旧权重结清。
     * 不结清的话这段时间会被追溯按新权重计价：一个任务在自己正跑着的时候把 nice
     * 从 0 调到 +10（权重 1024 → 110），换权重前跑的那几微秒会被按 110 计成 9 倍多的
     * vruntime，一次就足以把它顶到红黑树很靠后的位置、长时间轮不到。sched_test 的
     * 公平性用例正是这么用的（worker 自己给自己设 nice），实测低权重 worker 因此
     * 时好时坏地只拿到 1~5 次而不是应得的 20 次左右。Linux 的 reweight_entity()
     * 同样是先 update_curr() 再改权重。
     *
     * 只结算"正在本 CPU 上跑的那个任务"：其余任务被换下时已在 sched_schedule() 里
     * 结算过，它们的 proc_exec_start 是陈旧值，拿来算 delta 会得到一个巨大的假账。 */
    if (p == proc_get_current())
    {
        fair_update_curr(p);
    }

    p->proc_nice = nice;
    p->proc_weight = fair_nice_to_weight(nice);
    /* 余数是"按旧权重还没换算完的那部分"，换了权重就没有意义了，丢掉。
     * 它恒小于旧权重，最多损失不到一格 vruntime。 */
    p->proc_vruntime_rem = 0;

    if (p->proc_on_rq)
    {
        sched_dequeue(p);
        sched_enqueue(p);
    }

    spinlock_release(&run_queue.lock, run_queue_lock_key);
}

void sched_preempt_if_needed(void)
{
    pcb_t *curr = proc_get_current();
    /* cpua.S 的 trap_return 无条件调用本函数，每次 trap 返回前都会走一遍；
     * 本 hart 尚未跑到 proc_init() 时 curr 为 NULL（同 sched_task_tick()，见那里注释），
     * 此时没有"当前任务"可言，自然也谈不上要不要抢占，直接跳过。 */
    if (curr == NULL)
    {
        return;
    }
    if (curr->need_resched)
    {
        sched_schedule();
    }
}

void sched_setscheduler(pcb_t *p, int policy, uint8_t rt_prio)
{
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);

    bool was_on_rq = p->proc_on_rq;
    if (was_on_rq) /* 从旧队列中删除 */
    {
        sched_dequeue(p);
    }
    p->proc_policy = policy;
    p->proc_rt_priority = rt_prio;
    p->proc_sched_class = (policy == SCHED_NORMAL) ? &fair_sched_class : &rt_sched_class;
    if (was_on_rq)  /* 添加到新队列中 */
    {
        sched_enqueue(p);
    }

    spinlock_release(&run_queue.lock, run_queue_lock_key);
}
