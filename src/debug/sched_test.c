/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/**
 * @file sched_test.c
 * @brief 调度器回归测试（CFS/idle 类、生命周期、公平性）+ 全套测试聚合入口
 *
 * @details
 *   由 init 进程（pid 1，正规调度任务）在 DEBUG_SCHED_TEST 打开时调用 run_sched_tests()。
 *   init 作为协作式测试驱动：fork 出 worker 内核线程，worker 跑完自己的逻辑后 return
 *   （经 kernel_thread_entry 落到 do_exit），init 用 do_wait 收割，从而端到端触发
 *   do_fork / sched_activate / sched_schedule / do_exit / wakeup / do_wait 整条链路。
 *
 *   worker 通过全局变量把结果回传给 init（单核协作式，无真正并发，用 volatile 表意）。
 */

#include "console.h"
#include "proc.h"
#include "sched.h"
#include "cpu.h"
#include "tick.h"
#include "ktime.h"
#include "atomic.h"
#include "sync.h"

/* ============================================================
 * 共享断言计数器（sync_test.c / rt_sched_test.c 也用，故非 static）
 * ============================================================ */
int sched_test_pass = 0;
int sched_test_fail = 0;

void sched_test_check(const char *name, int cond)
{
    if (cond)
    {
        printf("  [PASS] %s\n", name);
        sched_test_pass += 1;
    }
    else
    {
        printf("  [FAIL] %s\n", name);
        sched_test_fail += 1;
    }
}

/* 收割当前进程的全部子进程，返回收割到的个数。
 * do_wait(-1) 在没有任何子进程时返回负值（ENO17_NO_CHILD），据此结束。 */
int sched_test_reap_all(void)
{
    int reaped = 0;
    while (1)
    {
        int status = 0;
        int16_t cpid = do_wait(-1, &status, 0);
        if (cpid <= 0)
        {
            break;
        }
        reaped += 1;
    }
    return reaped;
}

/* ============================================================
 * 单元测试：nice→权重表、sched_now 单调
 * 直接读写字段，不触碰就绪队列（idle 不在队列里，改 nice 不会 dequeue/enqueue）
 * ============================================================ */
static void sched_unit_tests(void)
{
    printf("\n-- unit: nice->weight & sched_now --\n");

    /* 用 idle_proc 做被测对象：它从不在就绪队列里，sched_set_nice 只改字段、
     * 不走 dequeue/enqueue 分支，测完恢复，绝对安全。 */
    pcb_t *idle = cpu_get_current()->idle_proc;
    int saved_nice = idle->proc_nice;

    sched_set_nice(idle, 0);
    sched_test_check("weight(nice 0) == 1024", idle->proc_weight == 1024);
    sched_set_nice(idle, -20);
    sched_test_check("weight(nice -20) == 88761", idle->proc_weight == 88761);
    sched_set_nice(idle, 19);
    sched_test_check("weight(nice 19) == 15", idle->proc_weight == 15);
    /* 越界应被钳制到 [-20, 19] */
    sched_set_nice(idle, 100);
    sched_test_check("nice clamped high to 19", idle->proc_nice == 19);
    sched_set_nice(idle, -100);
    sched_test_check("nice clamped low to -20", idle->proc_nice == -20);

    sched_set_nice(idle, saved_nice);

    uint64_t a = sched_now();
    uint64_t b = sched_now();
    sched_test_check("sched_now monotonic", b >= a);
}

/* ============================================================
 * 生命周期测试：do_fork → 运行 → return→do_exit → do_wait 收割
 * ============================================================ */
#define LIFE_N 4
static volatile int life_ran[LIFE_N];

static void *life_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;
    life_ran[idx] = 1;
    printf("  [worker %d] ran (pid=%d)\n", idx, proc_get_current()->proc_pid);
    return NULL; /* 退出码 0 */
}

static void *life_worker_code42(void *arg)
{
    (void)arg;
    return (void *)(intptr_t)42; /* 退出码 42：验证 do_wait 的 status 编码 */
}

static void sched_lifecycle_test(void)
{
    printf("\n-- lifecycle: fork / exit / wait --\n");

    for (int i = 0; i < LIFE_N; i++)
    {
        life_ran[i] = 0;
    }
    for (int i = 0; i < LIFE_N; i++)
    {
        int16_t p = create_kernel_thread_by_fork(life_worker, (void *)(intptr_t)i, 0);
        sched_test_check("fork worker ok", p > 0);
    }

    int reaped = sched_test_reap_all();
    sched_test_check("reaped 4 workers", reaped == LIFE_N);

    int all_ran = 1;
    for (int i = 0; i < LIFE_N; i++)
    {
        if (!life_ran[i])
        {
            all_ran = 0;
        }
    }
    sched_test_check("all 4 workers ran", all_ran);

    /* 单独验证退出码经 status 正确回传：WEXITSTATUS(status) = (status>>8)&0xff */
    create_kernel_thread_by_fork(life_worker_code42, NULL, 0);
    int status = 0;
    int16_t c = do_wait(-1, &status, 0);
    sched_test_check("reaped exit(42) child", c > 0);
    sched_test_check("exit code 42 via status", ((status >> 8) & 0xff) == 42);
}

/* ============================================================
 * CFS 公平性测试：一组 worker 抢一个共享预算，低 nice（高权重）应拿到更多
 *
 * SMP 说明：worker 数必须**多于 hart 数**，否则每个 worker 各占一个 hart、
 * 根本不存在 CPU 竞争，nice 权重也就无从体现（2 个 worker + 2 个 hart 时
 * 双方都能跑满，测不出任何公平性）。另外所有 worker 必须**全部创建完毕后
 * 才允许开跑**：hart1 会在 sched_activate 的 IPI 之后立刻把先建好的 worker
 * 调度起来，不设门槛的话它会在后面的 worker 还没被 fork 出来之前就把预算吃光。
 * ============================================================ */
#define CFS_WORKERS (2 * CORE_NUMBER) /* 保证可运行任务数 > hart 数，制造真实竞争 */

/* 这个用例**只测权重是否生效，不测有没有人被饿死**——后者由等权重的
 * sched_cfs_nostarve_test() 负责。这条分工是 2026-08-31 查那个断续出现的 73/74
 * （`all CFS workers ran` 偶发失败）时定下来的，原因值得写清楚，免得又被合回去：
 *
 * nice-10 权重 9548、nice+10 权重 110，四个 worker 总权重 2*9548 + 2*110 = 19316，
 * 每个低权重 worker 的应得份额只有 110/19316 = 0.57%。预算 400 时期望只有 2.3 次，
 * "至少 1 次"直接压在噪声底上。但把预算加到 4000 之后**次数一点没变**，还是 2——
 * 份额与预算无关，那是被饿死而不是分得少的特征，顺着这条线查出了两个真的内核 bug
 * （vruntime 整数截断、改 nice 前不结算，均已修，见 sched.c）。
 *
 * 修完之后用次数仍然测不准，根因是**观测量选错了**：CFS 分配的是 CPU 时间，而
 * 轮转次数 = 时间 / 单轮成本；单轮成本取决于那次让出有没有真的发生 switch_to——
 * 重权重 worker 让出后往往被重新选中，走"不真正切换"的快路径，每轮约 1.2 µs；
 * 低权重 worker 每次轮到都要付一次真正的 switch_to（含 satp 写入 + sfence.vma，
 * QEMU 下实测约 100 µs）。差 40 倍，于是次数由开销而非权重决定，实测出现过
 * nice-10 拿 691、nice+10 拿 3309 的**反转**。
 *
 * 改成断言累计 CPU 时间之后，实测比值稳定在 70~90 倍（理论 86.8），余量充足。
 * 次数只留着打印和"预算是否耗尽"用。无饥饿则交给等权重的用例，那里四个 worker
 * 行为对称、单轮成本一致，次数才是可信的观测量。 */
#define CFS_BUDGET  4000

static volatile int cfs_budget;
static volatile int cfs_count[CFS_WORKERS];
static volatile uint64_t cfs_rt[CFS_WORKERS]; /* 测量阶段内各自累计的真实 CPU 时间 */

/* 起跑线闸门（rt_sched_test.c 也用，故非 static）：worker 阻塞在这上面，由 init
 * **一次性广播**放行。
 * 两条要求都是踩出来的，改动前请先看完：
 *
 * ① **不能忙等**。原先写的是 `while (!cfs_start) sched_schedule();`——忙等是真的在
 *    烧 CPU，每个 worker 烧掉多少取决于它落在哪个 hart、跟谁抢，实测能差三个数量级
 *    （一个 worker 在屏障上累计 13 毫秒，另一个只有 50 微秒）。烧得多的那个进入测量
 *    阶段时 vruntime 已经背了十几万的债，一次都轮不到。**这是屏障污染了起跑线，
 *    不是调度器不公平**——CFS 让多吃了 CPU 的任务等，恰恰是对的。阻塞则不累积运行
 *    时间，唤醒重新入队时 fair_enqueue 把 vruntime 统一钳到 min_vruntime，起跑线才齐。
 *
 * ② **放行必须是一次广播，不能逐个唤醒**。用信号量试过（init 连调 CFS_WORKERS 次
 *    sem_up）：前两个被唤醒的 worker 会立刻把 init 抢下 CPU，等 init 再跑起来放行
 *    后两个时，预算早被前两个吃光——实测出现过等权重下 `counts: 208 192 0 0`，
 *    以及加权用例里轻权重独自跑了 4.4 毫秒导致 CPU 时间反转。waitq_wake_all 在持锁
 *    状态下一次唤醒全部，没有这个窗口。
 *
 * 也试过 sched_sleep_ticks 轮询：起跑线同样齐，但受 tick 粒度拖累，单次运行从
 * 0.5 秒涨到 12 秒以上，不值。 */
static osslock_t    cfs_gate_lock;
static waitq_t      cfs_gate_wq;
static volatile int cfs_gate_open;

void sched_test_gate_init(void)
{
    spinlock_init(&cfs_gate_lock);
    waitq_init(&cfs_gate_wq);
    cfs_gate_open = 0;
}

void sched_test_gate_wait(void)
{
    irq_key_t cfs_gate_lock_key = spinlock_acquire(&cfs_gate_lock);
    while (!cfs_gate_open)
    {
        waitq_prepare(&cfs_gate_wq);
        spinlock_release(&cfs_gate_lock, cfs_gate_lock_key);
        sched_schedule();
        /* 重新取锁：赋值给循环外的 key，不能再声明一个同名局部把它遮蔽掉 */
        cfs_gate_lock_key = spinlock_acquire(&cfs_gate_lock);
    }
    spinlock_release(&cfs_gate_lock, cfs_gate_lock_key);
}

void sched_test_gate_release(void)
{
    irq_key_t cfs_gate_lock_key = spinlock_acquire(&cfs_gate_lock);
    cfs_gate_open = 1;
    waitq_wake_all(&cfs_gate_wq);
    spinlock_release(&cfs_gate_lock, cfs_gate_lock_key);
}

/* 前一半 nice-10（权重 9548），后一半 nice+10（权重 110），相差约 87 倍 */
static int cfs_nice_of(int idx)
{
    return (idx < CFS_WORKERS / 2) ? -10 : +10;
}

static void *cfs_fair_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;

    /* 等所有 worker 都被 fork 出来再开跑，避免先建好的把预算独吞。
     * 在设 nice 之前等，让各 worker 阻塞阶段的权重一致。闸门为什么必须是"阻塞 +
     * 一次广播放行"而不是忙等或逐个唤醒，见 sched_test_gate_* 上方的说明。 */
    sched_test_gate_wait();

    /* 对自己设 nice：running 任务不在队列里，on_rq=false，只改字段，安全 */
    sched_set_nice(proc_get_current(), cfs_nice_of(idx));

    uint64_t rt0 = proc_get_current()->proc_sum_exec_runtime;

    while (1)
    {
        /* 预算是跨 hart 共享的，必须原子领取：两个 hart 真并行时，
         * 非原子的"读—减—写"会丢更新，预算总数对不上 */
        if (atomic_add(&cfs_budget, -1) <= 0)
        {
            atomic_add(&cfs_budget, 1); /* 领超了，还回去 */
            break;
        }
        cfs_count[idx] += 1; /* 每个 worker 只写自己那格，无竞争 */
        sched_schedule();    /* 主动让出，仍 RUNNING → 被重新入队 */
    }

    cfs_rt[idx] = proc_get_current()->proc_sum_exec_runtime - rt0;
    return NULL;
}

static void sched_cfs_fairness_test(void)
{
    printf("\n-- CFS fairness: nice weighting --\n");

    cfs_budget = CFS_BUDGET;
    sched_test_gate_init();
    for (int i = 0; i < CFS_WORKERS; i++)
    {
        cfs_count[i] = 0;
        cfs_rt[i] = 0;
    }

    for (int i = 0; i < CFS_WORKERS; i++)
    {
        create_kernel_thread_by_fork(cfs_fair_worker, (void *)(intptr_t)i, 0);
    }
    sched_test_gate_release(); /* 全部就位，一次广播放行 */
    sched_test_reap_all();

    int low_cnt = 0, high_cnt = 0;
    uint64_t low_rt = 0, high_rt = 0;
    for (int i = 0; i < CFS_WORKERS; i++)
    {
        if (cfs_nice_of(i) < 0)
        {
            low_cnt += cfs_count[i];
            low_rt  += cfs_rt[i];
        }
        else
        {
            high_cnt += cfs_count[i];
            high_rt  += cfs_rt[i];
        }
    }

    printf("  workers=%d  counts: nice-10=%d nice+10=%d   cputime: nice-10=%d nice+10=%d\n",
           CFS_WORKERS, low_cnt, high_cnt, (int)low_rt, (int)high_rt);

    sched_test_check("budget fully consumed", low_cnt + high_cnt == CFS_BUDGET);
    /* **断言 CPU 时间而不是轮转次数**。CFS 分配的是时间；轮转次数 = 时间 / 单轮成本，
     * 而单轮成本取决于该次让出有没有真的发生 switch_to（快路径约 1.2 µs，真切换约
     * 100 µs，差 40 倍），跟权重无关。用次数断言时实测出现过 nice-10 拿 691、
     * nice+10 拿 3309 的反转——不是调度器错了，是这个观测量选错了。 */
    sched_test_check("lower nice got more CPU time", low_rt > high_rt);
}

/* ============================================================
 * 无饥饿测试：**等权重**下每个 worker 都必须被调度到，且份额大致均等
 *
 * 与上面的公平性用例分工：那边比例悬殊（87:1），单轮成本又不对称，测得出"权重生效"
 * 但测不出"没人被饿死"；这边全部 nice 0，四个 worker 行为对称、单轮成本一致，
 * 于是"每人都跑到"和"份额均等"都成为稳稳可测的性质。真有人拿到 0，就是调度器
 * 漏掉了就绪队列里的某个任务，那才是需要查的 bug。
 * ============================================================ */
static void *cfs_nostarve_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;

    sched_test_gate_wait();

    while (1)
    {
        if (atomic_add(&cfs_budget, -1) <= 0)
        {
            atomic_add(&cfs_budget, 1);
            break;
        }
        cfs_count[idx] += 1;
        sched_schedule();
    }
    return NULL;
}

#define NOSTARVE_BUDGET 4000
/* 均等份额是 400/4 = 100；取 1/8 的下限（12）留足抖动余量，同时仍能抓住
 * "某个 worker 被系统性冷落"这类真问题 */
#define NOSTARVE_MIN    (NOSTARVE_BUDGET / CFS_WORKERS / 8)

static void sched_cfs_nostarve_test(void)
{
    printf("\n-- CFS no-starvation: equal weights --\n");

    cfs_budget = NOSTARVE_BUDGET;
    sched_test_gate_init();
    for (int i = 0; i < CFS_WORKERS; i++)
    {
        cfs_count[i] = 0;
    }

    for (int i = 0; i < CFS_WORKERS; i++)
    {
        create_kernel_thread_by_fork(cfs_nostarve_worker, (void *)(intptr_t)i, 0);
    }
    sched_test_gate_release();
    sched_test_reap_all();

    int total = 0;
    int all_ran = 1;
    int all_fair = 1;
    for (int i = 0; i < CFS_WORKERS; i++)
    {
        total += cfs_count[i];
        if (cfs_count[i] <= 0)
        {
            all_ran = 0;
        }
        if (cfs_count[i] < NOSTARVE_MIN)
        {
            all_fair = 0;
        }
    }

    printf("  counts:");
    for (int i = 0; i < CFS_WORKERS; i++)
    {
        printf(" %d", cfs_count[i]);
    }
    printf("   total=%d\n", total);

    sched_test_check("every equal-weight worker ran", all_ran);
    sched_test_check("no equal-weight worker starved", all_fair);
    sched_test_check("no-starve budget fully consumed", total == NOSTARVE_BUDGET);
}

/* ============================================================
 * 定时唤醒测试：sched_sleep_ticks 到点被 tick 中断唤醒，
 * 且睡眠期间不占 CPU——同时跑的 bg worker 应该能继续被调度到
 * ============================================================ */
#define TIMER_SLEEP_TICKS 3

static volatile int timer_bg_ran;
static volatile int timer_sleep_done;
static volatile uint64_t timer_slept_ns;

static void *timer_bg_worker(void *arg)
{
    (void)arg;
    while (!timer_sleep_done)
    {
        timer_bg_ran += 1;
        sched_schedule(); /* 主动让出，仍 RUNNING → 被重新入队 */
    }
    return NULL;
}

/* 用 ktime_get_ns()（直读硬件 time CSR）而不是 tick_get_os_tick() 来量。
 *
 * 原来用的是 cpu0 的**软件 tick 计数**，那个量里混了三样东西：睡眠开始时距下一次
 * tick 边界的相位、被唤醒后到真正跑起来的调度延迟、以及 tick 本身的抖动。于是
 * "睡 3 个 tick"量出来 3 或 4 都是正常的，偶尔还会是 2——4 核浸泡 200 次里中过 1 次
 * （0.5%），当时看着像内核早醒，实际内核完全正确：sched_sleep_ticks 换算成绝对纳秒，
 * sched_check_timers 也是拿 ktime_get_ns() 比对，真实睡眠时长从来没短过。
 *
 * 换成同一个时基之后，断言才真正在验"睡够了没有"。测量窗口仍然包含唤醒到运行的
 * 延迟，但那只会让 elapsed 变大，不影响 >= 这个方向。 */
static void *timer_sleep_worker(void *arg)
{
    (void)arg;
    uint64_t before = ktime_get_ns();
    sched_sleep_ticks(TIMER_SLEEP_TICKS);
    timer_slept_ns = ktime_get_ns() - before;
    timer_sleep_done = 1;
    return NULL;
}

static void sched_timed_sleep_test(void)
{
    printf("\n-- timed wakeup: sched_sleep_ticks --\n");

    const uint64_t want_ns = (uint64_t)TIMER_SLEEP_TICKS * (NSEC_PER_SEC / TICK_HZ);

    timer_bg_ran = 0;
    timer_sleep_done = 0;
    timer_slept_ns = 0;

    create_kernel_thread_by_fork(timer_bg_worker, NULL, 0);
    create_kernel_thread_by_fork(timer_sleep_worker, NULL, 0);
    sched_test_reap_all();

    printf("  slept %ld us (requested %ld us), bg worker ran %d times meanwhile\n",
           timer_slept_ns / 1000, want_ns / 1000, timer_bg_ran);
    sched_test_check("sleeper slept at least the requested duration",
                      timer_slept_ns >= want_ns);
    sched_test_check("bg worker kept running during sleep (not a busy-wait)",
                      timer_bg_ran > 0);
}

/* ============================================================
 * 聚合入口：由 init（DEBUG_SCHED_TEST）调用
 * ============================================================ */
void run_sched_tests(void)
{
    /* 其他测试文件的入口 */
    extern void sync_sem_wakeup_test(void);
    extern void sync_mutex_test(void);
    extern void rt_preempt_cfs_test(void);
    extern void rt_rr_rotation_test(void);
    extern void waitq_single_wakeup_test(void);
    extern void waitq_broadcast_test(void);
    extern void run_pipe_tests(void);

    printf("\n======== SCHEDULER REGRESSION TESTS ========\n");
    sched_test_pass = 0;
    sched_test_fail = 0;

    sched_unit_tests();
    sched_lifecycle_test();
    sched_cfs_fairness_test();
    sched_cfs_nostarve_test();
    sched_timed_sleep_test();
    sync_sem_wakeup_test();
    sync_mutex_test();
    rt_preempt_cfs_test();
    rt_rr_rotation_test();
    waitq_single_wakeup_test();
    waitq_broadcast_test();
    run_pipe_tests();

    printf("\n======== SCHED TESTS DONE: %d pass  %d fail ========\n\n",
           sched_test_pass, sched_test_fail);
}
