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
#include "atomic.h"

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
        int16_t cpid = do_wait(-1, &status);
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
    int16_t c = do_wait(-1, &status);
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

static volatile int cfs_budget;
static volatile int cfs_count[CFS_WORKERS];
static volatile int cfs_start; /* 0 = 所有 worker 就位前不许动预算 */

/* 前一半 nice-10（权重 9548），后一半 nice+10（权重 110），相差约 87 倍 */
static int cfs_nice_of(int idx)
{
    return (idx < CFS_WORKERS / 2) ? -10 : +10;
}

static void *cfs_fair_worker(void *arg)
{
    int idx = (int)(intptr_t)arg;

    /* 等所有 worker 都被 fork 出来再开跑，避免先建好的把预算独吞。
     * 在设 nice 之前等，让各 worker 空转阶段的权重一致，不污染公平性测量 */
    while (!cfs_start)
    {
        sched_schedule();
    }

    /* 对自己设 nice：running 任务不在队列里，on_rq=false，只改字段，安全 */
    sched_set_nice(proc_get_current(), cfs_nice_of(idx));

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
    return NULL;
}

static void sched_cfs_fairness_test(void)
{
    printf("\n-- CFS fairness: nice weighting --\n");

    cfs_budget = 400;
    cfs_start = 0;
    for (int i = 0; i < CFS_WORKERS; i++)
    {
        cfs_count[i] = 0;
    }

    for (int i = 0; i < CFS_WORKERS; i++)
    {
        create_kernel_thread_by_fork(cfs_fair_worker, (void *)(intptr_t)i, 0);
    }
    cfs_start = 1; /* 全部就位，放行 */
    sched_test_reap_all();

    int low_nice = 0;  /* nice-10 一组（高权重）合计 */
    int high_nice = 0; /* nice+10 一组（低权重）合计 */
    int all_ran = 1;
    for (int i = 0; i < CFS_WORKERS; i++)
    {
        if (cfs_count[i] <= 0)
        {
            all_ran = 0;
        }
        if (cfs_nice_of(i) < 0)
        {
            low_nice += cfs_count[i];
        }
        else
        {
            high_nice += cfs_count[i];
        }
    }

    printf("  workers=%d  nice-10 total=%d   nice+10 total=%d\n",
           CFS_WORKERS, low_nice, high_nice);
    sched_test_check("all CFS workers ran", all_ran);
    sched_test_check("budget fully consumed", low_nice + high_nice == 400);
    sched_test_check("lower nice got more CPU", low_nice > high_nice);
}

/* ============================================================
 * 定时唤醒测试：sched_sleep_ticks 到点被 tick 中断唤醒，
 * 且睡眠期间不占 CPU——同时跑的 bg worker 应该能继续被调度到
 * ============================================================ */
#define TIMER_SLEEP_TICKS 3

static volatile int timer_bg_ran;
static volatile int timer_sleep_done;
static volatile uint64_t timer_slept_ticks;

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

static void *timer_sleep_worker(void *arg)
{
    (void)arg;
    uint64_t before = tick_get_os_tick();
    sched_sleep_ticks(TIMER_SLEEP_TICKS);
    timer_slept_ticks = tick_get_os_tick() - before;
    timer_sleep_done = 1;
    return NULL;
}

static void sched_timed_sleep_test(void)
{
    printf("\n-- timed wakeup: sched_sleep_ticks --\n");

    timer_bg_ran = 0;
    timer_sleep_done = 0;
    timer_slept_ticks = 0;

    create_kernel_thread_by_fork(timer_bg_worker, NULL, 0);
    create_kernel_thread_by_fork(timer_sleep_worker, NULL, 0);
    sched_test_reap_all();

    printf("  slept %ld ticks (requested %d), bg worker ran %d times meanwhile\n",
           timer_slept_ticks, TIMER_SLEEP_TICKS, timer_bg_ran);
    sched_test_check("sleeper woke up after requested ticks",
                      timer_slept_ticks >= TIMER_SLEEP_TICKS);
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
