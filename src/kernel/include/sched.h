/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _SCHED_H_
#define _SCHED_H_

#include <stdint.h>

#include "rbtree.h"
#include "list.h"
#include "sync.h"
#include "proc.h"
#include "tick.h"

/**
 * @brief 读取调度用的当前时刻
 * @return `time` CSR 的当前值（频率见 tick.h 的 TIMEBASE_FREQ_HZ，按平台不同）
 * @details 有意不用 tick 计数（`tick_get_current()`）作为时基：即使按 200 Hz（5 ms/tick）
 *   折算，协作式调度下内核线程往往打印完就让出，运行时长远小于一个 tick ——
 *   那样所有任务的 delta 恒为 0、vruntime 全为 0，红黑树排序完全退化。
 *   直接读 `time` CSR 可获得约 0.1 微秒的分辨率，vruntime 记账才有意义。
 *   实际读取委托给 `tick_read_time()`（`tick.h`），避免和 tick 模块重复实现
 *   同一段内联汇编；这里单独留一个语义化的名字 + 这段调度侧的设计说明。
 */
static inline uint64_t sched_now(void)
{
    return tick_read_time();
}

#define SCHED_NICE_MIN (-20)
#define SCHED_NICE_MAX 19
/* nice=0 对应的基准权重（与 Linux 一致）；Δvruntime = delta * nice0 / proc_weight */
#define SCHED_NICE_0_WEIGHT 1024u

/* 唤醒抢占阈值（按10MHz折算，约10 ms）：被唤醒任务的 vruntime 至少领先这么多才值得抢占，避免抖动切换 */
#define SCHED_WAKEUP_GRANULARITY 100000UL
/* 最小运行粒度（按10MHz折算，约20 ms）：当前任务至少跑满这么久才允许被 tick 抢占 */
#define SCHED_MIN_GRANULARITY 200000UL

/**
 * @brief CFS 子队列：按 vruntime 排序的红黑树
 */
typedef struct cfs_run_queue
{
    struct rb_root tasks;   /* 按 vruntime 排序，最左即 vruntime 最小者 */
    uint64_t min_vruntime;  /* 单调递增下界，新建/唤醒任务的 vruntime 基准 */
    uint32_t nr_running;    /* 树中任务数（不含当前正在运行任务与 idle） */
} cfs_rq_t;

/* RT 优先级档数：数值越大优先级越高，bitmap 为 uint32_t 故上限 32 */
#define RT_MAX_PRIO 32
#define RT_SCHED_RR_TIMESLICE 200000UL

/**
 * @brief RT 子队列：每优先级一条就绪链表 + 位图（FreeRTOS 风格，O(1) 选择）
 */
typedef struct rt_run_queue
{
    struct list_head ready_lists[RT_MAX_PRIO]; /* 每优先级一条就绪链表 */
    uint32_t bitmap;                           /* 位p置1表示 ready_lists[p] 非空 */
    uint32_t nr_running;
} rt_rq_t;

/**
 * @brief 全局就绪队列：容纳所有调度类的子队列，共用一把锁
 */
typedef struct run_queue
{
    cfs_rq_t cfs;    /* CFS 子队列 */
    rt_rq_t rt;      /* RT 子队列 */
    osslock_t lock;  /* 保护整个rq，含两个子队列 */
} rq_t;

/**
 * @brief 调度类接口（vtable）：CFS（fair）与 RT 各实现一份，通过类链遍历选择
 */
typedef struct sched_class
{
    const struct sched_class *next;                 /* 类链：指向优先级更低的下一个类，NULL 表示链尾 */
    void   (*enqueue_task)(pcb_t *p);
    void   (*dequeue_task)(pcb_t *p);
    pcb_t *(*pick_next_task)(void);                  /* 本类无可运行任务则返回 NULL */
    void   (*task_tick)(pcb_t *curr);
    void   (*check_preempt)(pcb_t *curr, pcb_t *p);  /* p 唤醒时是否该抢占 curr */
} sched_class_t;

extern const sched_class_t rt_sched_class;    /* RT 类：rt_rq，优先级链表 + bitmap + RR 轮转 */
extern const sched_class_t fair_sched_class;  /* CFS 类：cfs_rq，rbtree + vruntime */
extern const sched_class_t idle_sched_class;  /* idle 类：链尾兜底，pick_next 恒返回本 CPU 的 idle_proc */

/* 初始化就绪队列（两个子队列 + 锁）；须在 proc_init() 之前调用 */
void sched_init(void);
/* 登记为本 CPU 的当前任务并记下换入时刻（proc_init() 与 sched_schedule() 调用） */
void sched_set_current(pcb_t *p);
/* 核心调度：结算当前任务、沿类链选出下一个任务并 switch_to 过去 */
void sched_schedule(void);
/* 被 switch_to 换上之后必须调用一次：释放前一条执行流持有的 run_queue.lock
 * （"接力"约定，见 sched_schedule 内注释）。新建执行流的入口 fork_out() 同样要调。 */
void sched_finish_switch(void);
/* 入就绪队列，已在队中则为空操作；调用者须持 run_queue.lock */
void sched_enqueue(pcb_t *p);
/* 出就绪队列，不在队中则为空操作；调用者须持 run_queue.lock */
void sched_dequeue(pcb_t *p);
/* p 刚变为就绪（唤醒/新建）：入队 + 判断是否该抢占当前任务，两步在同一把锁下完成；
 * do_fork/wakeup 调这一个函数即可，不要自己拆开调 sched_enqueue */
void sched_activate(pcb_t *p);
/* 时钟节拍调用：cfs 需要结算 vruntime，rt需要判断时间片是否用尽。并按需置位 need_resched */
void sched_task_tick(void);
/* 修改 nice 值并同步权重；若任务在树中则重新入树以更新排序键 */
void sched_set_nice(pcb_t *p, int nice);
void sched_preempt_if_needed(void);
void sched_setscheduler(pcb_t *p, int policy, uint8_t rt_prio);

/* 唤醒 proc：置 RUNNING + sched_activate。proc 是已知的目标任务（比如信号量
 * wait_list 里摘下来的那个、或父进程持有的子进程指针），不是"从某个队列弹出"。 */
void wakeup(pcb_t *proc);
/**
 * @brief 标记当前任务不可运行并让出 CPU，不维护等待队列
 * @param[in] proc  只能是 proc_get_current()：sched_schedule() 换下的永远是当前任务，
 *                  传别的 pcb 只会误改一个不相关任务的状态
 * @param[in] state INTERRUPTIBLE 或 UNINTERRUPTIBLE，由调用者按语义决定
 * @note 将来由谁 wakeup() 这个任务是调用者的责任。调用者须在自己的锁保护下完成
 *   "检查条件 → 挂入等待结构 → sleep()"，否则会在检查完条件、真正睡下之前被人
 *   抢先唤醒而丢失这次唤醒。
 */
void sleep(pcb_t *proc, sta_t state);

/* 当前任务定时睡眠 ns 纳秒后被 tick 中断唤醒；期间从就绪队列摘下，不占用 CPU
 * （区别于 tick_delay() 的忙等自旋）。到期检查点是每个 tick 一次，所以实际
 * 睡眠时长会向上取整到 tick 边界；纳秒时基买到的是"剩余时间可以算准"。
 * 可被信号唤醒（置 INTERRUPTIBLE）——返回后调用者应自行检查 signal_pending()。 */
void sched_sleep_ns(uint64_t ns);
/* 同上，以 tick 为单位。 */
void sched_sleep_ticks(uint64_t ticks);
/* 把任务从 sleeping_tasks 上摘下来；幂等（不在链上时是空操作）。
 * 被信号唤醒的定时睡眠必须走这里：signal_send() 只把任务置 RUNNING 并入就绪
 * 队列，节点仍留在 sleeping_tasks 上，到点 sched_check_timers() 会对同一个 pcb
 * 再 list_del + wakeup 一次（若进程已退出，pcb 已还给 slab，就是拿悬空指针操作链表）。 */
void sched_timer_remove(pcb_t *p);
/* 由 tick_int_handler() 每次 tick 调用：唤醒 sleeping_tasks 中已到期的任务。 */
void sched_check_timers(void);

#endif /* _SCHED_H_ */
