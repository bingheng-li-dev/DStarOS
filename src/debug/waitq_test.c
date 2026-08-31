/**
 * @file waitq_test.c
 * @brief 等待队列（waitq_t）回归测试：prepare 阻塞睡眠 / wake_all 唤醒、广播不丢
 *
 * @details 由 run_sched_tests()（sched_test.c）调用。waitq_t 是 pipe_read/pipe_write
 *   阻塞语义的唯一唤醒原语，必须独立验证过，否则后续任何"挂死"都无法判断是
 *   waitq 本身的问题还是管道自己的逻辑错误。套路与 sync_test.c 的 sem_down/sem_up
 *   测试一致：init 作为驱动任务，通过 sched_schedule 主动让出等 worker 跑到阻塞点。
 */

#include "console.h"
#include "proc.h"
#include "sched.h"
#include "sync.h"
#include "list.h"
#include "cpu.h"

/* SMP 下等待另一个 hart 上的 worker 到达某个阶段时，主动让出的最大次数上限；
 * 取够大以免误判，又不至于在真出问题时把测试挂死 */
#define WAITQ_YIELD_SPINS 10000

extern void sched_test_check(const char *name, int cond);
extern int sched_test_reap_all(void);

/* ============================================================
 * 测试一：单等待者 waitq_prepare 阻塞 + waitq_wake_all 唤醒
 * ============================================================ */
static osslock_t wq_lock;   /* 条件锁：wq_condition 与 wq_single 共用同一把 */
static waitq_t wq_single;
static volatile int wq_condition; /* 0=不成立 1=成立，由 init 在持锁状态下置位 */
static volatile int wq_stage;     /* 0=未开始 1=已到 prepare 前（即将阻塞） 2=醒来后越过循环 */

static void *wq_waiter(void *arg)
{
    (void)arg;
    irq_key_t wq_lock_key = spinlock_acquire(&wq_lock);
    while (!wq_condition)
    {
        wq_stage = 1;
        waitq_prepare(&wq_single);
        spinlock_release(&wq_lock, wq_lock_key);
        /* 被 waitq_wake_all 唤醒后从这里继续，回到循环开头重新检查条件——
         * 不能想当然直接成功，这正是 waitq_prepare 文档要求的用法 */
        sched_schedule();
        /* 重新取锁：赋值给循环外的 key，不能再声明一个同名局部把它遮蔽掉 */
        wq_lock_key = spinlock_acquire(&wq_lock);
    }
    wq_stage = 2;
    spinlock_release(&wq_lock, wq_lock_key);
    return NULL;
}

void waitq_single_wakeup_test(void)
{
    printf("\n-- waitq: single waiter prepare / wake_all wakeup --\n");

    spinlock_init(&wq_lock);
    waitq_init(&wq_single);
    wq_condition = 0;
    wq_stage = 0;
    create_kernel_thread_by_fork(wq_waiter, NULL, 0);

    /* 让出 CPU 等 waiter 上台置 stage=1、随后在 waitq_prepare 上阻塞睡眠。
     * SMP 下"让出一次"不保证对方已经跑过，改成有次数上限地反复让出，
     * 直到看见 stage=1；若它始终没跑到，循环耗尽后断言照样失败。 */
    for (int spin = 0; spin < WAITQ_YIELD_SPINS && wq_stage == 0; spin++)
    {
        sched_schedule();
    }
    sched_test_check("waiter reached waitq_prepare and blocked", wq_stage == 1);

    irq_key_t wq_lock_key = spinlock_acquire(&wq_lock);
    wq_condition = 1;
    waitq_wake_all(&wq_single);
    spinlock_release(&wq_lock, wq_lock_key);

    int status = 0;
    int16_t c = do_wait(-1, &status);
    sched_test_check("waiter reaped after wake_all", c > 0);
    sched_test_check("waiter proceeded past waitq_prepare", wq_stage == 2);
}

/* ============================================================
 * 测试二：多等待者广播唤醒——验证 wake_all 不丢、不误伤、不重复唤醒
 * ============================================================ */
#define WQ_BROADCAST_N 3
static osslock_t wqb_lock;
static waitq_t wqb;
static volatile int wqb_condition;
static volatile int wqb_woken_count; /* 已越过 waitq_prepare 循环的 worker 数 */

static void *wqb_waiter(void *arg)
{
    (void)arg;
    irq_key_t wqb_lock_key = spinlock_acquire(&wqb_lock);
    while (!wqb_condition)
    {
        waitq_prepare(&wqb);
        spinlock_release(&wqb_lock, wqb_lock_key);
        sched_schedule();
        /* 重新取锁：赋值给循环外的 key，不能再声明一个同名局部把它遮蔽掉 */
        wqb_lock_key = spinlock_acquire(&wqb_lock);
    }
    wqb_woken_count += 1;
    spinlock_release(&wqb_lock, wqb_lock_key);
    return NULL;
}

void waitq_broadcast_test(void)
{
    printf("\n-- waitq: broadcast wake_all wakes every waiter, none lost --\n");

    spinlock_init(&wqb_lock);
    waitq_init(&wqb);
    wqb_condition = 0;
    wqb_woken_count = 0;

    for (int i = 0; i < WQ_BROADCAST_N; i++)
    {
        create_kernel_thread_by_fork(wqb_waiter, NULL, 0);
    }

    /* 等所有 worker 都跑到阻塞点：直接数 wqb.task_list 上挂了几个节点，
     * 比轮询某个计数器更直接地验证"确实都到了 waitq_prepare 之后"。 */
    int queued = 0;
    for (int spin = 0; spin < WAITQ_YIELD_SPINS; spin++)
    {
        sched_schedule();
        irq_key_t wqb_lock_key = spinlock_acquire(&wqb_lock);
        queued = 0;
        struct list_head *pos;
        list_for_each(pos, &(wqb.task_list))
        {
            queued++;
        }
        spinlock_release(&wqb_lock, wqb_lock_key);
        if (queued == WQ_BROADCAST_N)
        {
            break;
        }
    }
    sched_test_check("all 3 waiters queued on waitq", queued == WQ_BROADCAST_N);

    irq_key_t wqb_lock_key = spinlock_acquire(&wqb_lock);
    wqb_condition = 1;
    waitq_wake_all(&wqb);
    spinlock_release(&wqb_lock, wqb_lock_key);

    int reaped = sched_test_reap_all();
    sched_test_check("reaped all 3 broadcast workers", reaped == WQ_BROADCAST_N);
    sched_test_check("wake_all woke every waiter (none lost)", wqb_woken_count == WQ_BROADCAST_N);
}
