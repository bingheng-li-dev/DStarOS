/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "tick.h"
#include "console.h"
#include "sync.h"
#include "sbi.h"
#include "debug.h"
#include "encoding.h"
#include "sched.h"
#include "tty.h"
#include "ktime.h"
#include "fdt.h"

#define OS_TICK cpu_get_by_index(0)->tick

osslock_t tick_lock;
/* 频率常量与 tick 频率见 tick.h 的 TIMEBASE_FREQ_HZ / TICK_HZ——那里也解释了
 * 两个平台的折算值为何不可混用。clock_gettime 需要的是频率本身，所以常量必须
 * 有名字、不能只以除法结果的形式存在。 */
static uint64_t timebase = TICK_PERIOD_COUNTS;

static void tick_set_next_int(uint64_t stime)
{
    sbi_set_timer(tick_read_time() + stime);
#if DEBUG_TICK
    printf("++ setup timer interrupts\n");
#endif
}

void tick_check_timebase(void)
{
    if (!fdt_is_available())
    {
        printf("timebase: %lu Hz (compile-time; no usable dtb to verify against)\n",
               TIMEBASE_FREQ_HZ);
        return;
    }

    const void *cpus = fdt_find_node("/cpus");
    uint32_t hz = 0;
    if (cpus == NULL || !fdt_prop_u32(cpus, "timebase-frequency", &hz))
    {
        printf("timebase: %lu Hz (compile-time; dtb has no /cpus/timebase-frequency)\n",
               TIMEBASE_FREQ_HZ);
        return;
    }

    if ((uint64_t)hz == (uint64_t)TIMEBASE_FREQ_HZ)
    {
        printf("timebase: %lu Hz (dtb confirms)\n", (unsigned long)hz);
        return;
    }

    /* 不自动改用 DTB 的值：TIMEBASE_FREQ_HZ 是编译期常量，被 ktime.h 的内联函数与
     * syscall.c 当除数用（常量除法才会被优化成乘加移位），运行时改不了。
     * 这里只负责把"猜错了"这件事喊得足够响——否则它的症状是打字发粘、sleep 时长
     * 整体偏，很容易被当成别的问题查半天。 */
    printf("timebase: *** MISMATCH *** compile-time %lu Hz, dtb says %lu Hz\n",
           TIMEBASE_FREQ_HZ, (unsigned long)hz);
    printf("timebase: tick rate and all sleep durations will be off by %lu/%lu"
           " -- fix TIMEBASE_FREQ_HZ in tick.h and rebuild\n",
           (unsigned long)hz, TIMEBASE_FREQ_HZ);
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
    ktime_check_alarms();
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
