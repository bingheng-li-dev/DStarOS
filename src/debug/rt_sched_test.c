/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

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
#include "atomic.h"

extern void sched_test_check(const char *name, int cond);
extern int sched_test_reap_all(void);
/* 广播门闩：实现在 sched_test.c，两个测试文件共用（为什么必须是"阻塞 + 一次广播放行"
 * 而不是忙等或逐个唤醒，见那边的说明） */
extern void sched_test_gate_init(void);
extern void sched_test_gate_wait(void);
extern void sched_test_gate_release(void);

/* ============================================================
 * 测试一：RT 抢占 CFS —— 只要还有 RT 任务可运行，CFS 就分不到 CPU
 *
 * **RT worker 数必须等于 hart 数**。就绪队列是全局唯一的，两个 hart 各自从中取任务；
 * 只有 1 个 RT 任务时，它占住一个 hart，另一个 hart 空着自然会去跑 CFS 任务——
 * 那是正确行为（不能因为别处有 RT 任务就让一个 CPU 闲着），不是抢占失效。
 * 占满所有 hart，"CFS 一次都跑不到"才成为一条可断言的性质。
 *
 * 【2026-08-31 修】原来只开 1 个 RT worker，断言 `rt_last < cfs_first`（RT 全程跑完
 * 才轮到 CFS）。那要求严格串行，在双 hart 上根本不成立，能长期通过只是因为 RT worker
 * 那 10 次迭代太快、hart1 通常来不及把 CFS worker 调上来——实测余量恰好 1 格
 * （cfs_first 总是 rt_last+1），约 0.3% 的运行会翻。人为放大 fork 与提升 RT 之间的
 * 窗口后必现 `rt[1..22] cfs[3..13]`，即两个 worker 在两个 hart 上并行跑。
 *
 * 另一处一并修掉：原来靠"init 在 fork 与提升 RT 之间不让出"来保证 worker 不会抢跑。
 * 这个前提同样不成立——fork 里的 sched_activate 会 IPI 另一个 hart，worker 可能在
 * 还是 CFS 身份时就被调度起来。改成让 worker 阻塞在广播门闩上，init 设完调度类再放行。
 * ============================================================ */
#define RTP_RT_WORKERS  CORE_NUMBER  /* 占满所有 hart，见上方说明 */
#define RTP_ITERS       10

static volatile int rtp_seq;
static volatile int rtp_rt_done[RTP_RT_WORKERS]; /* 各 RT worker 跑完那一刻的序号 */
static volatile int rtp_cfs_first;               /* CFS worker 第一次拿到 CPU 的序号 */
static volatile int rtp_cfs_last;

/* 多个 worker 在不同 hart 上并发领号，必须原子自增；atomic_add 返回旧值 */
static int rtp_next_seq(void)
{
    return atomic_add(&rtp_seq, 1) + 1;
}

static void *rtp_rt_worker(void *arg)
{
    int id = (int)(intptr_t)arg;

    sched_test_gate_wait();

    for (int k = 0; k < RTP_ITERS; k++)
    {
        rtp_next_seq();
        sched_schedule(); /* RR 让出：让出前会被重新入队，本优先级里仍是它，立刻被选回 */
    }
    rtp_rt_done[id] = rtp_next_seq();
    return NULL;
}

static void *rtp_cfs_worker(void *arg)
{
    (void)arg;

    sched_test_gate_wait();

    rtp_cfs_first = rtp_next_seq();
    for (int k = 1; k < RTP_ITERS; k++)
    {
        rtp_cfs_last = rtp_next_seq();
        sched_schedule();
    }
    return NULL;
}

void rt_preempt_cfs_test(void)
{
    printf("\n-- RT preempts CFS --\n");

    rtp_seq = 0;
    rtp_cfs_first = 0;
    rtp_cfs_last = 0;
    for (int i = 0; i < RTP_RT_WORKERS; i++)
    {
        rtp_rt_done[i] = 0;
    }
    sched_test_gate_init();

    int16_t rp[RTP_RT_WORKERS];
    for (int i = 0; i < RTP_RT_WORKERS; i++)
    {
        rp[i] = create_kernel_thread_by_fork(rtp_rt_worker, (void *)(intptr_t)i, 0);
    }
    create_kernel_thread_by_fork(rtp_cfs_worker, NULL, 0);

    /* 门闩关着，谁也没开跑，这里从容把 RT worker 全部设成 SCHED_RR */
    int found_all = 1;
    for (int i = 0; i < RTP_RT_WORKERS; i++)
    {
        pcb_t *w = proc_find_by_pid(rp[i]);
        if (w == NULL)
        {
            found_all = 0;
        }
        else
        {
            sched_setscheduler(w, SCHED_RR, 10);
        }
    }
    sched_test_check("found all RT worker pcbs", found_all);

    sched_test_gate_release(); /* 一次广播放行：RT 与 CFS 同时变为可运行 */
    sched_test_reap_all();

    /* 第一个 RT worker 退出的那一刻，才空出第一个 hart */
    int first_rt_done = rtp_rt_done[0];
    for (int i = 1; i < RTP_RT_WORKERS; i++)
    {
        if (rtp_rt_done[i] < first_rt_done)
        {
            first_rt_done = rtp_rt_done[i];
        }
    }

    printf("  最先跑完的 RT worker 在序号 %d 退出，CFS 活动区间[%d..%d]\n",
           first_rt_done, rtp_cfs_first, rtp_cfs_last);
    sched_test_check("both RT and CFS workers ran",
                     first_rt_done > 0 && rtp_cfs_first > 0);
    /* 所有 hart 都被 RT 占着的那段时间里，CFS 一次都不该跑到 */
    sched_test_check("CFS got no CPU while RT occupied every hart",
                     rtp_cfs_first > first_rt_done);
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

static void *rr_worker(void *arg)
{
    int id = (int)(intptr_t)arg;

    /* 开跑前先阻塞在门闩上，等 init 把所有 worker 都设成同优先级 RR 之后再放行。
     * 有 IPI 之后 hart1 会在 worker 刚被 fork 出来时立刻调度它，没有门闩的话它会
     * 以 CFS 身份一口气把 8 轮跑完，序列里只剩"一个跑到底再换下一个"，看不到轮转。
     *
     * 门闩有两条硬要求，都在 sched_test_gate_* 上方有详细说明：
     *   ① 必须"阻塞等"而不是"自旋等"——init 稍后会把这些 worker 设成 SCHED_RR，
     *      RT 任务只要可运行就永远压过 CFS 身份的 init，自旋等会让 init 再也拿不到
     *      CPU 去放行，直接活锁；
     *   ② 放行必须是一次广播。这里原先用信号量、init 连调 RR_WORKERS 次 sem_up，
     *      而第一个被唤醒的 RR worker 就是 RT 任务，立刻把 init 顶下 CPU——剩下的
     *      sem_up 要等到某个 worker 退出才做得成，最后一个 worker 迟到加入，轮转
     *      被测到的只剩后半段。waitq_wake_all 在持锁状态下一次唤醒全部，没有这个窗口。 */
    sched_test_gate_wait();

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
    sched_test_gate_init();

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

    /* 全部设完 RR 之后一次广播放行，这样它们是"同时"变成可运行的同优先级 RT 任务，
     * 谁也没有抢跑的机会，轮转才是被真正测到的 */
    sched_test_gate_release();

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
