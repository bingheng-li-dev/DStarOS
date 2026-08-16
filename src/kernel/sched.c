#include "sched.h"
#include "trap.h"
#include "proc.h"
#include "rbtree.h"
#include "stringops.h"
#include "dassert.h"
#include "tick.h"
#include "cpu.h"

/* 按 proc_wake_tick 升序排列的定时睡眠链表，sched_sleep_ticks()/sched_check_timers() 共用 */
static struct list_head sleeping_tasks;
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

    /* Δvruntime = delta * nice0 / proc_weight */
    curr->proc_vruntime += delta * SCHED_NICE_0_WEIGHT / curr->proc_weight;

    /* 比较rbtree最左节点进程vruntime和当前进程vruntime，
     * 选择更小的那个与旧的min_vruntime比较，择其大者 */
    uint64_t min_vruntime = curr->proc_vruntime;
    if (run_queue.cfs.nr_running > 0)
    {
        pcb_t *leftmost = rb_entry(rb_first(&run_queue.cfs.tasks), pcb_t, proc_rbtree_node);
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

    if (run_queue.cfs.nr_running > 0)
    {
        pcb_t *leftmost = rb_entry(rb_first(&run_queue.cfs.tasks), pcb_t, proc_rbtree_node);
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
    p->proc_exec_start = sched_now();
    /* 换入快照：往后 fair_task_tick 用 proc_sum_exec_runtime - 这份快照
     * 算出"这次换上 CPU 以来跑了多久"，判断是否已跑满 SCHED_MIN_GRANULARITY */
    p->proc_sum_exec_runtime_prev = p->proc_sum_exec_runtime;
}

void sched_schedule(void)
{
    spinlock_acquire(&run_queue.lock);

    pcb_t *curr = proc_get_current();
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

    /* 在调用switch_to之前锁必须释放：
     * switch_to之前的代码是上一个执行流，后面要等到某次调度把它换回来才会继续
     * 下一个调度决策需要先acquire锁，却永远等不到，除非又调度那个执行流 */
    if (next != curr)
    {
        spinlock_unlock((spinlock_t *)&run_queue.lock);  /* 只放锁，中断仍关着 */
        switch_to(&curr->proc_context, &next->proc_context);
        /* curr 这条执行流将来被换回来时，从这里继续往下跑 */
    }
    else
    {
        spinlock_release(&run_queue.lock);  /* 没真正切换，正常放锁+恢复中断 */
        return;
    }

    irq_disable_nesting_decrement(); /* 这个语句是切换后的执行流执行的 */
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
    spinlock_acquire(&run_queue.lock);

    sched_enqueue(p);
    /* p 是刚变为就绪的外部任务，
     * 拿它和当前正在跑的任务比一次，看要不要立刻抢占 */
    check_preempt_curr(proc_get_current(), p);

    spinlock_release(&run_queue.lock);

    /* 本来这里应该在这个 hart 正好是别的 wfi 空转的 hart 该去跑的任务时，
     * 用 cpu_send_ipi() 主动踢醒它，不用等下一次 tick 中断才发现就绪队列
     * 里多了东西。已实现（发送端 cpu_send_ipi() / sbi.h 的标准 IPI 扩展，
     * 接收端 trap.c 的 IRQ_S_SOFT 清 sip.SSIP）但触发概率很高地引出一个
     * 尚未定位的时序 bug（现象记录在 .claude/bugfixes.md），暂时不在这里
     * 调用 cpu_send_ipi()——hart1 目前只靠 tick 中断周期性醒来轮询就绪
     * 队列，最坏发现延迟一个 tick，功能上仍然正确。 */
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
 * @param[in] ticks 要睡眠的 tick 数（tick_get_os_tick() 单位）
 * @details 在 sleeping_tasks_lock 保护下把 proc_state 置 INTERRUPTIBLE
 *   并按 proc_wake_tick 升序插入 sleeping_tasks，随后才 sched_schedule()。
 *   状态赋值与入链在同一把锁下完成，是为了不丢唤醒——万一 ticks 极小，
 *   插入后、sched_schedule() 真正切换走前就被 sched_check_timers() 抢先
 *   唤醒（proc_state 改回 RUNNING 并入就绪队列），sched_schedule() 发现
 *   当前任务已是 RUNNING，会按"主动让出"处理而不会重复入队，不会丢事件。
 * @note 与 tick_delay() 的忙等自旋不同，本函数会真正让出 CPU。
 */
void sched_sleep_ticks(uint64_t ticks)
{
    pcb_t *curr = proc_get_current();
    uint64_t wake_at = tick_get_os_tick() + ticks;

    spinlock_acquire(&sleeping_tasks_lock);

    curr->proc_wake_tick = wake_at;
    curr->proc_state = INTERRUPTIBLE;

    struct list_head *pos;
    list_for_each(pos, &sleeping_tasks)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_timer_linker);
        if (p->proc_wake_tick > wake_at)
        {
            break;
        }
    }
    list_add_tail(&curr->proc_timer_linker, pos);

    spinlock_release(&sleeping_tasks_lock);

    sched_schedule();
}

/**
 * @brief 唤醒 sleeping_tasks 中所有已到期的任务
 * @details 由 tick_int_handler() 每次 tick 调用。sleeping_tasks 按
 *   proc_wake_tick 升序排列，一旦遇到未到期的节点即可停止扫描。
 * @note wakeup() 内部会取 run_queue.lock；本函数持有的 sleeping_tasks_lock
 *   在所有路径上都只会是外层锁（sched_sleep_ticks() 插入时早已释放它，
 *   之后才单独去拿 run_queue.lock），不会与 run_queue.lock 形成加锁顺序反转。
 */
void sched_check_timers(void)
{
    uint64_t now = tick_get_os_tick();
    struct list_head *pos, *tmp;

    spinlock_acquire(&sleeping_tasks_lock);
    list_for_each_safe(pos, tmp, &sleeping_tasks)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_timer_linker);
        if (p->proc_wake_tick > now)
        {
            break;
        }
        list_del(&p->proc_timer_linker);
        wakeup(p);
    }
    spinlock_release(&sleeping_tasks_lock);
}

void sched_task_tick(void)
{
    pcb_t *curr = proc_get_current();
    curr->proc_sched_class->task_tick(curr);
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

    spinlock_acquire(&run_queue.lock);

    p->proc_nice = nice;
    p->proc_weight = fair_nice_to_weight(nice);

    if (p->proc_on_rq)
    {
        sched_dequeue(p);
        sched_enqueue(p);
    }

    spinlock_release(&run_queue.lock);
}

void sched_preempt_if_needed(void)
{
    pcb_t *curr = proc_get_current();
    if (curr->need_resched)
    {
        sched_schedule();
    }
}

void sched_setscheduler(pcb_t *p, int policy, uint8_t rt_prio)
{
    spinlock_acquire(&run_queue.lock);

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

    spinlock_release(&run_queue.lock);
}
