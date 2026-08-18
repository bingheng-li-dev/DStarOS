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
#include "sync.h"
#include "cpu.h"
#include "atomic.h"

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
/* SMP 说明：RR worker 数必须**多于 hart 数**，否则每个 worker 各占一个 hart，
 * 谁也不用等谁，根本不发生轮转。同理，"严格交替、无两个连续相同"在 SMP 下不再是
 * 有意义的不变式——两个 worker 真的在两个 hart 上同时跑，谁先写下自己那笔纯属时序，
 * 出现相邻相同完全正常。改为检验"确实在轮转"：每个 worker 都跑满，且序列中
 * worker 之间频繁换手（而不是一个先跑到底、另一个再开始）。 */
#define RR_WORKERS (CORE_NUMBER + 1)
#define RR_ITERS   8
#define RR_SLOTS   (RR_WORKERS * RR_ITERS)

static volatile int rr_seq[RR_SLOTS];
static volatile int rr_n;
static ossem_t rr_gate; /* count=0：worker 一上台就睡在这里，等 init 放行 */

static void *rr_worker(void *arg)
{
    int id = (int)(intptr_t)arg;

    /* 开跑前先睡在门闩上，等 init 把所有 worker 都设成同优先级 RR 之后再放行。
     * 有 IPI 之后 hart1 会在 worker 刚被 fork 出来时立刻调度它，没有门闩的话它会
     * 以 CFS 身份一口气把 8 轮跑完，序列里只剩"一个跑到底再换下一个"，看不到轮转。
     *
     * 门闩必须是"睡着等"（信号量）而不是"自旋等"：init 稍后会把这些 worker 设成
     * SCHED_RR，RT 任务只要可运行就永远压过 CFS 身份的 init，自旋等会让 init
     * 再也拿不到 CPU 去放行，直接活锁。 */
    sem_down(&rr_gate);

    for (int k = 0; k < RR_ITERS; k++)
    {
        /* 原子领取槽位：多个 hart 并行写同一个下标会互相覆盖、丢记录 */
        int slot = atomic_add(&rr_n, 1);
        if (slot < RR_SLOTS)
        {
            rr_seq[slot] = id;
        }
        sched_schedule(); /* 让出：RR 重新入队到本优先级链尾，另一个同级 RT 被选中 */
    }
    return NULL;
}

void rt_rr_rotation_test(void)
{
    printf("\n-- RT SCHED_RR round-robin --\n");

    rr_n = 0;
    sem_init(&rr_gate, 0);

    int16_t pid[RR_WORKERS];
    for (int i = 0; i < RR_WORKERS; i++)
    {
        pid[i] = create_kernel_thread_by_fork(rr_worker, (void *)(intptr_t)i, 0);
    }

    /* 必须在【任何一个运行之前】把所有 worker 都设成同优先级 RR，否则先跑的那个若已是 RR，
     * 会一直抢在还是 CFS 的其它 worker 前面、跑到退出，根本轮不到别人，无从轮转。 */
    int found_all = 1;
    for (int i = 0; i < RR_WORKERS; i++)
    {
        pcb_t *w = proc_find_by_pid(pid[i]);
        if (w == NULL)
        {
            found_all = 0;
        }
        else
        {
            sched_setscheduler(w, SCHED_RR, 5);
        }
    }
    sched_test_check("found all RR worker pcbs", found_all);

    /* 全部设完 RR 之后再一次性放行，这样它们是"同时"变成可运行的同优先级 RT 任务，
     * 谁也没有抢跑的机会，轮转才是被真正测到的 */
    for (int i = 0; i < RR_WORKERS; i++)
    {
        sem_up(&rr_gate);
    }

    sched_test_reap_all();

    int total = (rr_n < RR_SLOTS) ? rr_n : RR_SLOTS;
    int count[RR_WORKERS];
    for (int i = 0; i < RR_WORKERS; i++)
    {
        count[i] = 0;
    }
    for (int i = 0; i < total; i++)
    {
        count[rr_seq[i]] += 1;
    }

    /* 换手次数：序列里相邻两笔来自不同 worker 的次数。一个 worker 先跑到底再换下一个
     * （完全不轮转）只会有 RR_WORKERS-1 次；真正轮转则接近 total-1 次。
     * 取 total/2 作门槛，既能挡住"跑到底"的退化，又给 SMP 下的并行乱序留足余量。 */
    int handovers = 0;
    for (int i = 1; i < total; i++)
    {
        if (rr_seq[i] != rr_seq[i - 1])
        {
            handovers += 1;
        }
    }

    int all_ran = 1;
    for (int i = 0; i < RR_WORKERS; i++)
    {
        if (count[i] != RR_ITERS)
        {
            all_ran = 0;
        }
    }

    printf("  RR seq len=%d  handovers=%d  counts:", total, handovers);
    for (int i = 0; i < RR_WORKERS; i++)
    {
        printf(" w%d=%d", i, count[i]);
    }
    printf("\n");
    sched_test_check("all RR workers ran full iterations", all_ran);
    sched_test_check("RR rotates (frequent handovers, no run-to-completion)",
                     total > 0 && handovers >= total / 2);
}
