/**
 * @file sync_test.c
 * @brief 信号量回归测试：sem_down 阻塞睡眠 / sem_up 唤醒、以及互斥语义
 *
 * @details 由 run_sched_tests()（sched_test.c）调用。init 作为驱动任务，
 *   通过 sched_schedule 主动让出，让 worker 先跑到 sem_down 阻塞点，再 sem_up 唤醒，
 *   端到端验证 sleep(UNINTERRUPTIBLE)→sched_schedule 与 wakeup→sched_activate 这条链路。
 */

#include "console.h"
#include "proc.h"
#include "sched.h"
#include "sync.h"
#include "cpu.h"

extern void sched_test_check(const char *name, int cond);
extern int sched_test_reap_all(void);

/* ============================================================
 * 测试一：sem_down 阻塞 + sem_up 唤醒
 * ============================================================ */
static ossem_t sem_sync;
/* 0=未开始 1=已到 sem_down 前（即将阻塞） 2=已越过 sem_down（被唤醒后继续） */
static volatile int consumer_stage;

static void *sem_consumer(void *arg)
{
    (void)arg;
    consumer_stage = 1;
    sem_down(&sem_sync); /* count=0 → 阻塞睡眠，直到 init sem_up */
    consumer_stage = 2;
    return NULL;
}

void sync_sem_wakeup_test(void)
{
    printf("\n-- semaphore: sem_down sleep / sem_up wakeup --\n");

    sem_init(&sem_sync, 0);
    consumer_stage = 0;
    create_kernel_thread_by_fork(sem_consumer, NULL, 0);

    /* 让出一次：consumer 被调度上台，置 stage=1，随后 sem_down 阻塞睡眠，
     * 调度回到 init（init 让出时仍 RUNNING 被重新入队，故会被选回）。 */
    sched_schedule();
    sched_test_check("consumer reached sem_down and blocked", consumer_stage == 1);

    /* 唤醒 consumer：count→1 且等待队列非空 → wakeup(consumer) */
    sem_up(&sem_sync);

    /* 收割 consumer：它被唤醒后越过 sem_down（stage=2）→ return → do_exit */
    int status = 0;
    int16_t c = do_wait(-1, &status);
    sched_test_check("consumer reaped after wakeup", c > 0);
    sched_test_check("consumer proceeded past sem_down", consumer_stage == 2);
}

/* ============================================================
 * 测试二：以 sem(1) 作互斥锁，保护共享计数的读-改-写
 * 每个 worker 在临界区内主动让出，若互斥失效会丢更新，最终计数 < 期望
 * ============================================================ */
static ossem_t mtx;
static volatile int shared_counter;
#define MTX_WORKERS 2
#define MTX_ITERS   50

static void *mtx_worker(void *arg)
{
    (void)arg;
    for (int k = 0; k < MTX_ITERS; k++)
    {
        sem_down(&mtx); /* 进入临界区（互斥失效则两个 worker 会同时进来） */
        int tmp = shared_counter;
        sched_schedule();       /* 临界区内故意让出，制造交错机会 */
        shared_counter = tmp + 1;
        sem_up(&mtx);
        sched_schedule();       /* 临界区外也让出，增加调度交错 */
    }
    return NULL;
}

void sync_mutex_test(void)
{
    printf("\n-- semaphore: mutual exclusion (sem as mutex) --\n");

    sem_init(&mtx, 1);
    shared_counter = 0;

    for (int i = 0; i < MTX_WORKERS; i++)
    {
        create_kernel_thread_by_fork(mtx_worker, (void *)(intptr_t)i, 0);
    }
    sched_test_reap_all();

    printf("  shared_counter=%d (expect %d)\n", shared_counter, MTX_WORKERS * MTX_ITERS);
    sched_test_check("mutex kept counter correct", shared_counter == MTX_WORKERS * MTX_ITERS);
}
