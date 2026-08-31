#include "tick.h"
#include "console.h"
#include "sync.h"
#include "sbi.h"
#include "debug.h"
#include "encoding.h"
#include "sched.h"
#include "tty.h"

#define OS_TICK cpu_get_by_index(0)->tick

osslock_t tick_lock;
/* timebase 单位是 time CSR 的计数值，需按平台实际主频折算出 200 Hz（5 ms 一个 tick）：
 * QEMU virt 的 time CSR 为 10 MHz（-machine dumpdtb 导出的 timebase-frequency 实测确认）；
 * K210 无 time CSR，由 rustsbi-k210 模拟 CLINT mtime，频率对应其 390 MHz 主频折算。
 * 二者不可混用同一个常量——此前长期沿用 K210 的折算值在 QEMU 上运行，实际 tick 周期
 * 变成约 0.195 秒（~5 Hz），仅在此前从未依赖低延迟轮询的场景下未被察觉。
 * @todo 若日后硬件选型定为 VisionFive 2（JH7110），需按其 time CSR 实际频率
 *   （SoC 手册/设备树 timebase-frequency 实测值，不是 K210 的 390 MHz）新增一个
 *   平台分支，不可直接套用下面任一现有值。 */
#ifdef QEMU
static uint64_t timebase = (10000000 / 200);
#else
static uint64_t timebase = (390000000 / 200);
#endif

static void tick_set_next_int(uint64_t stime)
{
    sbi_set_timer(tick_read_time() + stime);
#if DEBUG_TICK
    printf("++ setup timer interrupts\n");
#endif
}

/* 必须在trap_init()之后被调用 */
void tick_init(void)
{
    /* tick_lock 是所有 hart 共享的一把锁，只能初始化一次 */
    if (cpu_get_core_id() == 0)
    {
        spinlock_init(&tick_lock);
    }
    tick_set_next_int(timebase);
    cpu_get_current()->tick = 0;
    printf("%s::core %d tick inited!\n", __FUNCTION__, cpu_get_core_id());
}

void tick_int_handler(void)
{
    irq_key_t tick_lock_key = spinlock_acquire(&tick_lock);
    cpu_get_current()->tick += 1;
#if DEBUG_TICK
    if (cpu_get_current()->tick % 100 == 0)
    {
        printf("core %ld : %ld ticks\n", cpu_get_current()->tick);
    }
#endif
    spinlock_release(&tick_lock, tick_lock_key);
    /* 以下几步都必须在 tick_lock 释放之后，它们各自要抢别的锁，叠在 tick_lock
     * 里面只会多一层没必要的锁序：
     *   sched_task_tick 要抢 run_queue.lock（它读就绪队列，必须与 enqueue/dequeue 互斥）；
     *   tty_poll_input 会抢 tty_lock 并可能触发 waitq_wake_all -> sched_wakeup -> 就绪队列锁。 */
    sched_task_tick();
    tty_poll_input();
    sched_check_timers();
    tick_set_next_int(timebase);
}

uint64_t tick_get_os_tick(void)
{
    return OS_TICK;
}

uint64_t tick_get_current(void)
{
    return cpu_get_current()->tick;
}

void tick_set_os_tick(uint64_t tick)
{
    //interrupt_disable
    OS_TICK = tick;
    //interrupt_enable
}

void tick_delay(uint64_t ticks)
{
    uint64_t tick_start = tick_get_os_tick();
    uint64_t tick;
    do
    {
        tick = tick_get_os_tick();
        if (tick - tick_start == ticks)
            return;
    } while (1);
}
