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
/* 频率常量见 tick.h 的 TIMEBASE_FREQ_HZ / TICK_HZ */
static uint64_t timebase = TICK_PERIOD_COUNTS;

static void tick_set_next_int(uint64_t stime)
{
    sbi_set_timer(tick_read_time() + stime);
#if DEBUG_TICK
    printf("++ setup timer interrupts\n");
#endif
}

/**
 * @brief 用设备树里的 /cpus/timebase-frequency 核对编译期常量 TIMEBASE_FREQ_HZ
 */
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

    /* 不自动改用 DTB 的值：TIMEBASE_FREQ_HZ 是编译期常量（被当除数用），运行时改不了，
     * 这里只负责把"猜错了"喊得足够响。 */
    printf("timebase: *** MISMATCH *** compile-time %lu Hz, dtb says %lu Hz\n",
           TIMEBASE_FREQ_HZ, (unsigned long)hz);
    printf("timebase: tick rate and all sleep durations will be off by %lu/%lu"
           " -- fix TIMEBASE_FREQ_HZ in tick.h and rebuild\n",
           (unsigned long)hz, TIMEBASE_FREQ_HZ);
}

/**
 * @brief 初始化本 hart 的时钟中断
 * @note 必须在 trap_init() 之后调用。
 */
void tick_init(void)
{
    /* tick_lock 是所有 hart 共享的一把锁，只能初始化一次 */
    if (cpu_get_core_id() == 0)
    {
        spinlock_init(&tick_lock);
    }
    tick_set_next_int(timebase);
    cpu_get_current()->tick = 0;
    printf("%s::core %d tick inited!\n", __func__, cpu_get_core_id());
}

/**
 * @brief 时钟中断处理：本 hart 的 tick 加一，并驱动调度、TTY 轮询与各类定时器
 */
void tick_int_handler(void)
{
    irq_key_t tick_lock_key = spinlock_acquire(&tick_lock);
    cpu_get_current()->tick += 1;
#if DEBUG_TICK
    if (cpu_get_current()->tick % 100 == 0)
    {
        printf("core %ld : %ld ticks\n", cpu_get_core_id(), cpu_get_current()->tick);
    }
#endif
    spinlock_release(&tick_lock, tick_lock_key);
    /* 以下几步都在 tick_lock 释放之后做，它们各自要抢别的锁：sched_task_tick 抢
     * run_queue.lock，tty_poll_input 抢 tty_lock 并可能经 wakeup 再抢就绪队列锁。 */
    sched_task_tick();
    tty_poll_input();
    sched_check_timers();
    ktime_check_alarms();
    tick_set_next_int(timebase);
}

/**
 * @brief 延时函数用的全局 tick，取核 0 的计数
 */
uint64_t tick_get_os_tick(void)
{
    return OS_TICK;
}

/**
 * @brief 本 hart 自己的 tick 计数（各 hart 各有定时器中断）
 */
uint64_t tick_get_current(void)
{
    return cpu_get_current()->tick;
}

/**
 * @brief 设置全局 tick（核 0 的计数）
 */
void tick_set_os_tick(uint64_t tick)
{
    OS_TICK = tick;
}

/**
 * @brief 核忙等待延时，以 tick 为单位
 */
void tick_delay(uint64_t ticks)
{
    uint64_t tick_start = tick_get_os_tick();
    uint64_t tick;
    do
    {
        tick = tick_get_os_tick();
        if (tick - tick_start == ticks)
        {
            return;
        }
    } while (1);
}
