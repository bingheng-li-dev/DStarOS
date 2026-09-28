/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _TICK_H_
#define _TICK_H_

#include <stdint.h>

#include "sync.h"

extern osslock_t tick_lock;

/* time CSR 的计数频率。必须与平台实际值一致：写错了不会报错，只会让 tick 周期与
 * 所有睡眠时长整体偏。
 *
 * 保持编译期常量而不是运行时变量，是因为它被 ktime.h 的内联函数与 syscall.c 当除数用，
 * 常量除法会被优化成乘加移位；改成变量就是每次 clock_gettime 都做一次真 64 位除法。
 * 代价是写错了只能重编——所以 tick_check_timebase() 在开机时核对它。 */
#if defined(QEMU)
#define TIMEBASE_FREQ_HZ 10000000UL     /* -machine dumpdtb 导出的 timebase-frequency 实测确认 */
#elif defined(VF2)
#define TIMEBASE_FREQ_HZ 4000000UL      /* JH7110，待上板核对；靠 tick_check_timebase() 的自检兜底 */
#else
#error "未知平台：TIMEBASE_FREQ_HZ 没有对应取值（PLATFORM 只允许 QEMU / VF2）"
#endif

/* 定时器中断频率（5 ms 一个 tick）。tty 的输入靠 tick 轮询，低于这个值打字会发粘。 */
#define TICK_HZ 200UL

/* 一个 tick 对应的 time CSR 计数值 */
#define TICK_PERIOD_COUNTS (TIMEBASE_FREQ_HZ / TICK_HZ)

/* 直接读硬件 time CSR（约 0.1 微秒分辨率），区别于下面 tick_get_os_tick()/
 * tick_get_current() 返回的软件 tick 计数（5 ms 一格，粒度粗得多）。 */
static inline uint64_t tick_read_time(void)
{
    uint64_t x;
    asm volatile("csrr %0, time"
                 : "=r"(x));
    return x;
}

void tick_init(void);
/**
 * @brief 用设备树里的 /cpus/timebase-frequency 核对上面那个编译期常量
 * @note 必须在 fdt_init() 之后、MMU 开启之前调用——DTB 不在内核偏移映射
 *   范围内，MMU 开启后再去读就是个没建过映射的地址。详见 fdt.h。
 * @note 只打印、不改行为：常量是唯一真相来源，DTB 只负责在猜错时喊一声。
 */
void tick_check_timebase(void);
void tick_int_handler(void);
/* 延时函数用的全局 tick，取核 0 的计数 */
uint64_t tick_get_os_tick(void);
/* 本 hart 自己的 tick 计数（各 hart 各有定时器中断） */
uint64_t tick_get_current(void);
void tick_set_os_tick(uint64_t tick);
/* 核忙等待延时，以tick为单位。 */
void tick_delay(uint64_t ticks);

#endif /* _TICK_H_ */
