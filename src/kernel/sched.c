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

/* 按 proc_wake_time_ns 升序排列的定时睡眠链表。
 * tick_int_handler() 无条件调用 sched_check_timers()，而定时器中断可能早于 sched_init()
 * 到达（200 Hz 下 hart0 挂载 ramdisk 就可能超过一个 tick），所以链表头必须在链接时刻
 * 就是合法空链表——BSS 清零得到的 next/prev 全 NULL 会让 list_for_each_safe 当场崩。 */
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
 * @details min_vruntime是为了防止一个刚创建或睡眠太久的进程，其拥有一个极小的vruntime，
 *   因此会报复性地长时间占用CPU。min_vruntime单调递增，强制给定一个min_vruntime最小值
 * @note 调用者须持有 run_queue.lock。
 * @note vruntime 溢出需连续运行数百年，不做回绕处理。
 */
static void fair_update_curr(pcb_t *curr)
{
    if (curr->proc_sched_class != &fair_sched_class)
    {
        return;
    }

    uint64_t now = sched_now();
    uint64_t delta = now - curr->proc_exec_start;
    curr->proc_exec_start = now;

    /* 未加权执行时间，只增不清零；减去换入快照即本次上 CPU 以来跑了多久 */
    curr->proc_sum_exec_runtime += delta;

    /* Δvruntime = delta * nice0 / proc_weight，除不尽的余数留到下次。
     * 频繁让出的任务每次 delta 只有几格，整数除法会把高权重任务的 Δvruntime 截断成 0，
     * 它的 vruntime 于是停在原地、独占 CPU，加权公平完全失效。
     * 余数累加回下一次，无论切片多短，时间都不会丢。 */
    uint64_t numerator = delta * SCHED_NICE_0_WEIGHT + curr->proc_vruntime_rem;
    curr->proc_vruntime     += numerator / curr->proc_weight;
    curr->proc_vruntime_rem  = (uint32_t)(numerator % curr->proc_weight);

    uint64_t min_vruntime = curr->proc_vruntime;
    /* 判空只能看树本身，不能看 nr_running——测一个、用另一个，两者一旦不同步
     * 就会把 rb_first() 的 NULL 当成有效节点。 */
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
    /* 入队钳制：新建任务的 vruntime 为 0，长睡任务的停在睡前，而 min_vruntime 一直前推；
     * 不钳制它们会以远低于当前最低的值插到最左、换入后长时间垄断 CPU。
     * 钳到 min_vruntime 只抬平过低的，不影响本就 >= min_vruntime 的任务。 */
    if (p->proc_vruntime < run_queue.cfs.min_vruntime)
    {
        p->proc_vruntime = run_queue.cfs.min_vruntime;
    }

    struct rb_node **link = &run_queue.cfs.tasks.rb_node;
    struct rb_node *parent = NULL;

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
    rb_link_node(&p->proc_rbtree_node, parent, link);
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
    uint64_t delta = now - curr->proc_exec_start;
    curr->proc_exec_start = now;
    curr->proc_sum_exec_runtime += delta;
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

/* idle 是各类都无就绪任务时的兜底，永远可运行，从不挂进任何队列 */
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
    /* 链尾兜底：走到这里说明 rt/fair 都无就绪任务，从不返回 NULL */
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
            curr->need_resched = true;
        }
        return;
    }
    /* 同类：委托给该类的 check_preempt（CFS 比 vruntime，RT 比 rt_priority） */
    curr->proc_sched_class->check_preempt(curr, p);
}

/**
 * @brief 初始化就绪队列（两个子队列 + 锁）；须在 proc_init() 之前调用
 */
void sched_init(void)
{
    spinlock_init(&run_queue.lock);
    run_queue.cfs.tasks = RB_ROOT;
    run_queue.cfs.min_vruntime = 0;
    run_queue.cfs.nr_running = 0;
    /* 链表头必须 INIT_LIST_HEAD 成自环，不能 memset 清零：
     * 清零后 prev 为 NULL，首次 list_add_tail 就会解引用它。 */
    for (int i = 0; i < RT_MAX_PRIO; i++)
    {
        INIT_LIST_HEAD(&run_queue.rt.ready_lists[i]);
    }
    run_queue.rt.bitmap = 0;
    run_queue.rt.nr_running = 0;

    spinlock_init(&sleeping_tasks_lock);
}

/**
 * @brief 登记为本 CPU 的当前任务并记下换入时刻（proc_init() 与 sched_schedule() 调用）
 */
void sched_set_current(pcb_t *p)
{
    cpu_get_current()->current_proc = p;
    p->proc_on_cpu = true;
    p->proc_exec_start = sched_now();
    /* 换入快照，fair_task_tick 据此判断本次上 CPU 后是否跑满 SCHED_MIN_GRANULARITY */
    p->proc_sum_exec_runtime_prev = p->proc_sum_exec_runtime;
}

/**
 * @brief 核心调度：结算当前任务、沿类链选出下一个任务并 switch_to 过去
 */
void sched_schedule(void)
{
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);

    pcb_t *curr = proc_get_current();
    /* 这把锁由"被换上的那条执行流"接力释放（见 sched_finish_switch），所以 key 要
     * 随任务走：存进自己的 pcb，将来自己被换回来时再取出来用。 */
    curr->proc_rq_key = run_queue_lock_key;

    /* 护栏：本 hart 连续调度这么多次而 tick 一格没动，说明时钟中断停了。
     * 其症状是整机静默卡死，只有当场 panic 才拿得到现场。 */
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

    /* 锁必须持有到 switch_to 把 curr 的上下文保存完：curr 已被放回共享就绪队列，
     * 此时放锁，另一个 hart 可能按尚未写入的旧上下文把它换上，同一任务就会在两个
     * hart 上共用一个内核栈并发执行。
     * 因此由被换上的执行流在 switch_to 之后用它自己 pcb 里的 proc_rq_key 放锁。 */
    if (next != curr)
    {
        /* curr 的 proc_on_cpu 要等 switch_to 写完上下文后才能清零，那时跑的已是
         * 被换上的执行流，所以经 per-hart 的 prev_proc 交给它（见 sched_finish_switch） */
        cpu_get_current()->prev_proc = curr;
        /* 浮点上下文只在真正切换时存取：这里存，由被换上的执行流在 sched_finish_switch()
         * 里恢复。next == curr 时两者都不做，否则会用旧值盖掉活着的寄存器。 */
        fpu_save(curr);
        switch_to(&curr->proc_context, &next->proc_context);
        /* curr 这条执行流将来被换回来时从这里继续；此刻持有的是"把它换回来的那条
         * 执行流"acquire 的锁，由下面这次 release 接力放掉 */
    }

    sched_finish_switch();
}

/**
 * @brief 被 switch_to 换上之后必须调用一次：释放前一条执行流持有的 run_queue.lock
 * @note 新建执行流的入口 fork_out() 同样要调。
 */
void sched_finish_switch(void)
{
    /* 此刻上一个任务的 proc_context 已由 switch_to 写完，才可以清它的 proc_on_cpu、
     * 允许别的 hart 挑走它。清零必须在放锁之前，才能与 sched_activate() 里的
     * proc_on_cpu 判断被同一把锁串行化。 */
    cpu_t *cpu = cpu_get_current();
    /* prev_proc 非空 <=> 刚真的换过任务，即该不该恢复浮点的判据。正常切换与 fork_out
     * （新任务不会从 switch_to 返回）都走这里；next == curr 时 prev_proc 为空。 */
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

    /* 用被换上的任务自己存下的 key，不能用 per-CPU 的值：与这次 release 配对的
     * acquire 发生在这条执行流当初被换出时，可能在另一个 hart 上。 */
    pcb_t *me = cpu->current_proc;
    spinlock_release(&run_queue.lock, me->proc_rq_key);
}

/**
 * @brief 入就绪队列，已在队中则为空操作；调用者须持 run_queue.lock
 */
void sched_enqueue(pcb_t *p)
{
    if (p->proc_on_rq)
    {
        return;
    }

    p->proc_sched_class->enqueue_task(p);
    p->proc_on_rq = true;
}

/**
 * @brief 出就绪队列，不在队中则为空操作；调用者须持 run_queue.lock
 */
void sched_dequeue(pcb_t *p)
{
    if (!p->proc_on_rq)
    {
        return;
    }

    p->proc_sched_class->dequeue_task(p);
    p->proc_on_rq = false;
}

/**
 * @brief p 刚变为就绪（唤醒/新建）：入队 + 判断是否该抢占当前任务，两步在同一把锁下完成
 */
void sched_activate(pcb_t *p)
{
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);

    /* p 可能还在另一个 hart 上跑（已标成待睡眠、放了条件锁，但还没到 switch_to），
     * 此时不能入队，否则会被别的 hart 按陈旧上下文换上。唤醒不会丢：状态已改回
     * RUNNING，它自己走到 sched_schedule 时会重新入队；两条路径由 run_queue.lock 串行化。 */
    bool enqueued = false;
    if (!p->proc_on_cpu)
    {
        sched_enqueue(p);
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
 * @brief 就绪队列新增任务后，IPI 踢醒正在 wfi 空转的其它 hart
 * @details 不踢的话，空转的 hart 最迟要等下一个 tick（5 ms）才会重新查看就绪队列。
 *
 *   刻意在 run_queue.lock 之外调用：发 IPI 是一次 SBI ecall，不该拉长持锁时间。
 *   这里不加锁读 current_proc/idle_proc 只是建议性的：多发一次对方会重新 wfi，
 *   漏发一次对方最迟下一个 tick 也会自己发现。
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

/**
 * @brief 唤醒 proc：置 RUNNING + sched_activate
 */
void wakeup(pcb_t *proc)
{
    proc->proc_state = RUNNING;
    sched_activate(proc);
}

/**
 * @brief 标记当前任务不可运行并让出 CPU，不维护等待队列
 */
void sleep(pcb_t *proc, sta_t state)
{
    dassert(proc == proc_get_current());
    proc->proc_state = state;
    sched_schedule();
}

/**
 * @brief 当前任务定时睡眠，到期由 tick 中断唤醒
 * @param[in] ns 要睡眠的纳秒数
 * @details 状态置 INTERRUPTIBLE 与按 proc_wake_time_ns 升序入链在同一把锁下完成，
 *   之后才 sched_schedule()：若 ns 极小、切走之前已被 sched_check_timers() 唤醒，
 *   sched_schedule() 看到 RUNNING 会按主动让出处理，不会丢唤醒。
 *
 *   返回前无条件摘一次链：到期路径已摘过，list_empty 判据会挡住第二次；
 *   被信号唤醒的路径这里是唯一的摘链点。与 waitq_remove 是同一种收口方式。
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

/**
 * @brief 同 sched_sleep_ns()，以 tick 为单位
 */
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
 * @note 锁序 sleeping_tasks_lock → run_queue.lock（wakeup 内部），单向：
 *   sched_sleep_ns() 放掉 sleeping_tasks_lock 之后才去拿 run_queue.lock。
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

/**
 * @brief 时钟节拍调用：cfs 需要结算 vruntime，rt需要判断时间片是否用尽。并按需置位 need_resched
 */
void sched_task_tick(void)
{
    pcb_t *curr = proc_get_current();
    /* 本 hart 还没走到 proc_init() 时定时器中断就可能到来，此时无任务可结算 */
    if (curr == NULL)
    {
        return;
    }

    /* task_tick 要读就绪队列（CFS 取最左节点），必须与别的 hart 的入队/出队互斥。
     * 锁序：本函数在 tick_lock 释放之后调用，不嵌套在 tick_lock 内。 */
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);
    curr->proc_sched_class->task_tick(curr);
    spinlock_release(&run_queue.lock, run_queue_lock_key);
}

/**
 * @brief 修改 nice 值并同步权重；若任务在树中则重新入树以更新排序键
 */
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

    /* 换权重前先按旧权重结清已跑、尚未计入 vruntime 的时间，否则那段会被追溯按
     * 新权重计价（nice 0 → +10 即多算 9 倍）。只结算本 CPU 上正在跑的任务：
     * 其余任务换下时已结算过，proc_exec_start 是陈旧值。 */
    if (p == proc_get_current())
    {
        fair_update_curr(p);
    }

    p->proc_nice = nice;
    p->proc_weight = fair_nice_to_weight(nice);
    /* 旧权重下的余数换权重后已无意义，丢弃最多损失不到一格 vruntime */
    p->proc_vruntime_rem = 0;

    if (p->proc_on_rq)
    {
        sched_dequeue(p);
        sched_enqueue(p);
    }

    spinlock_release(&run_queue.lock, run_queue_lock_key);
}

/**
 * @brief 当前任务被置了 need_resched 就调度一次；trap 返回前调用
 */
void sched_preempt_if_needed(void)
{
    pcb_t *curr = proc_get_current();
    /* trap_return 无条件调用本函数，本 hart 尚未到 proc_init() 时 curr 为 NULL */
    if (curr == NULL)
    {
        return;
    }
    if (curr->need_resched)
    {
        sched_schedule();
    }
}

/**
 * @brief 切换任务的调度策略与 RT 优先级，在队中则换到新类的队列
 */
void sched_setscheduler(pcb_t *p, int policy, uint8_t rt_prio)
{
    irq_key_t run_queue_lock_key = spinlock_acquire(&run_queue.lock);

    bool was_on_rq = p->proc_on_rq;
    if (was_on_rq)
    {
        sched_dequeue(p);
    }
    p->proc_policy = policy;
    p->proc_rt_priority = rt_prio;
    p->proc_sched_class = (policy == SCHED_NORMAL) ? &fair_sched_class : &rt_sched_class;
    if (was_on_rq)
    {
        sched_enqueue(p);
    }

    spinlock_release(&run_queue.lock, run_queue_lock_key);
}
