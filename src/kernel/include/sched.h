#ifndef _SCHED_H_
#define _SCHED_H_

#include <stdint.h>

#include "rbtree.h"
#include "list.h"
#include "sync.h"
#include "proc.h"

/**
 * @brief 读取调度用的当前时刻
 * @return `time` CSR 的当前值（QEMU virt 为 10 MHz，K210 约 7.8 MHz）
 * @details @TODO
 *   有意**不用** tick 计数（`getCurrentTick()`）作为时基：
 *   `TIMEBASE = 390000000 / 200` 意味着约 0.2 秒才有一个 tick，而协作式调度下
 *   内核线程往往打印完就让出，运行时长远小于一个 tick ——
 *   那样所有任务的 delta 恒为 0、vruntime 全为 0，红黑树排序完全退化。
 *   直接读 `time` CSR 可获得约 0.1 微秒的分辨率，vruntime 记账才有意义。
 */
static inline uint64_t sched_now(void)
{
    uint64_t t;
    asm volatile("csrr %0, time" : "=r"(t));
    return t;
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
    uint32_t nr_running;    /* 树中任务数（不含当前z正在运行任务与 idle） */
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
    cfs_rq_t cfs;    /* CFS 子队列（M1 启用） */
    rt_rq_t rt;      /* RT 子队列（M3 启用，M1 仅初始化） */
    osslock_t lock;  /* 保护整个rq，含两个子队列 */
} rq_t;

/**
 * @brief 调度类接口（vtable）：CFS（fair）与 RT 各实现一份，通过类链遍历选择
 * @note 与 `proc.h` 中 `pcb_t.proc_sched_class` 的前向声明 `struct sched_class` 是同一类型；
 *   此处补全定义。函数体（`fair_sched_class`/`rt_sched_class` 两份实例）由 sched.c 手动实现，
 *   这里只定义接口形状。
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
/* 登记为当前任务；由 proc_init() 调用 */
void sched_set_current(pcb_t *p);
/* 核心调度：结算当前任务 vruntime、选出最左任务并 switch_to 过去 */
void sched_schedule(void);
/* 入就绪队列（fork / wakeup 路径） */
void sched_enqueue(pcb_t *p);
/* 出就绪队列 */
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
/* 只负责"标记不可运行 + 让出 CPU"，不维护等待队列——谁负责将来找到并
 * wakeup() 这个任务是调用者的责任。调用者必须在自己的锁保护下完成
 * "检查条件 → 挂入等待结构 → sleep()"整套操作，否则会有检查完条件、
 * 真正睡下去之前被人抢先 wakeup 导致丢失唤醒的风险。
 * @param proc  只能是 getCurrentProc()——sched_schedule() 换下的永远是当前
 *              正在这个 CPU 上跑的任务，传别的 pcb 只会误改一个不相关任务
 *              的状态，当前任务该做的让出 CPU 却不会发生。
 * @param state 睡眠时置的状态，INTERRUPTIBLE 或 UNINTERRUPTIBLE，由调用者
 *              按自己的语义决定（比如信号量等待用 UNINTERRUPTIBLE）。 */
void sleep(pcb_t *proc, sta_t state);

#endif /* _SCHED_H_ */
