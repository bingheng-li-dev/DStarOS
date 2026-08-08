/**
 * @file rt_sched_test.c
 * @brief RT 调度类回归测试：跨类抢占（RT 优先于 CFS）、同优先级 SCHED_RR 轮转
 *
 * @details 由 run_sched_tests()（sched_test.c）调用。
 *   worker 的调度类由 init 在其运行之前用 sched_setscheduler 设好（需要在"被调度上台前"
 *   完成，故用 proc_find_by_pid 拿到 pcb，而非 worker 自己设——自己设会太晚，见 rr 测试注释）。
 */

#include "console.h"
#include "proc.h"
#include "sched.h"
#include "cpu.h"

extern void sched_test_check(const char *name, int cond);
extern int sched_test_reap_all(void);

/* ============================================================
 * 测试一：RT 抢占 CFS —— RT 任务只要就绪就永远排在 CFS 前面
 * 用一个全局递增序号记录两个 worker 的活动区间，RT 应完全跑在 CFS 之前
 * ============================================================ */
static volatile int seq;
static volatile int rt_first, rt_last, cfs_first, cfs_last;

static void *rtp_rt_worker(void *arg)
{
    (void)arg;
    rt_first = ++seq;
    for (int k = 0; k < 10; k++)
    {
        rt_last = ++seq;
        sched_schedule(); /* RR/FIFO 让出：RT 类里只有它，仍会被立刻选回 */
    }
    return NULL;
}

static void *rtp_cfs_worker(void *arg)
{
    (void)arg;
    cfs_first = ++seq;
    for (int k = 0; k < 10; k++)
    {
        cfs_last = ++seq;
        sched_schedule();
    }
    return NULL;
}

void rt_preempt_cfs_test(void)
{
    printf("\n-- RT preempts CFS --\n");

    seq = 0;
    rt_first = rt_last = cfs_first = cfs_last = 0;

    int16_t rp = create_kernel_thread_by_fork(rtp_rt_worker, NULL, 0);
    int16_t cp = create_kernel_thread_by_fork(rtp_cfs_worker, NULL, 0);
    (void)cp;

    /* 在两个 worker 被调度上台前，把 rp 提升为 RT（SCHED_RR，优先级 10）。
     * init 在 fork 与此处之间不让出，故 worker 不会提前运行。 */
    pcb_t *rt = proc_find_by_pid(rp);
    sched_test_check("found rt worker pcb", rt != NULL);
    if (rt)
    {
        sched_setscheduler(rt, SCHED_RR, 10);
    }

    sched_test_reap_all();

    printf("  rt活动区间[%d..%d]  cfs活动区间[%d..%d]\n", rt_first, rt_last, cfs_first, cfs_last);
    sched_test_check("both RT and CFS workers ran", rt_first > 0 && cfs_first > 0);
    /* RT 全程跑完（rt_last）才轮到 CFS 开始（cfs_first） */
    sched_test_check("RT fully ran before CFS started", rt_last < cfs_first);
}

/* ============================================================
 * 测试二：同优先级 SCHED_RR 轮转 —— 两个同优先级 RT 线程应交替执行
 * ============================================================ */
#define RR_SLOTS 32
static volatile int rr_seq[RR_SLOTS];
static volatile int rr_n;

static void *rr_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (int k = 0; k < 8; k++)
    {
        if (rr_n < RR_SLOTS)
        {
            rr_seq[rr_n++] = id;
        }
        sched_schedule(); /* 让出：RR 重新入队到本优先级链尾，另一个同级 RT 被选中 */
    }
    return NULL;
}

void rt_rr_rotation_test(void)
{
    printf("\n-- RT SCHED_RR round-robin --\n");

    rr_n = 0;

    int16_t p0 = create_kernel_thread_by_fork(rr_worker, (void *)(intptr_t)0, 0);
    int16_t p1 = create_kernel_thread_by_fork(rr_worker, (void *)(intptr_t)1, 0);

    /* 必须在【两者都运行之前】把两者都设成同优先级 RR，否则先跑的那个若已是 RR，
     * 会一直抢在还是 CFS 的另一个前面、跑到退出，根本轮不到另一个，无从轮转。 */
    pcb_t *w0 = proc_find_by_pid(p0);
    pcb_t *w1 = proc_find_by_pid(p1);
    sched_test_check("found both RR worker pcbs", w0 != NULL && w1 != NULL);
    if (w0 && w1)
    {
        sched_setscheduler(w0, SCHED_RR, 5);
        sched_setscheduler(w1, SCHED_RR, 5);
    }

    sched_test_reap_all();

    int c0 = 0, c1 = 0;
    for (int i = 0; i < rr_n; i++)
    {
        if (rr_seq[i] == 0)
        {
            c0 += 1;
        }
        else
        {
            c1 += 1;
        }
    }
    int alternates = 1;
    for (int i = 1; i < rr_n; i++)
    {
        if (rr_seq[i] == rr_seq[i - 1])
        {
            alternates = 0;
        }
    }

    printf("  RR sequence len=%d  w0=%d w1=%d\n", rr_n, c0, c1);
    sched_test_check("both RR workers ran", c0 > 0 && c1 > 0);
    sched_test_check("RR alternates (no two consecutive same)", alternates);
}
