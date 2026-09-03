#ifndef _KTIME_H_
#define _KTIME_H_

#include <stdint.h>

#include "tick.h"

struct proc_control_block;

#define NSEC_PER_SEC  1000000000UL
#define NSEC_PER_USEC 1000UL
#define USEC_PER_SEC  1000000UL

/* CLOCK_REALTIME 的起点。本机没有 RTC，墙钟只能从一个编译期常量起步、掉电即忘；
 * 用户随时可以用 clock_settime/settimeofday 把它拨到别处。
 * 1767225600 = 2026-01-01T00:00:00Z。 */
#define DSTAROS_BUILD_EPOCH_SEC 1767225600UL

/**
 * @brief time CSR 计数值换算成纳秒
 * @details 先把整秒摘出去再乘余数，否则 counts * 1e9 在 counts 超过约 1.8e10
 *   （QEMU 的 10 MHz 下约 30 分钟）时 64 位就溢出了。
 *   余数最大 FREQ-1，乘 1e9 后最坏 3.9e17（K210 的 390 MHz），离 1.8e19 还很远。
 * @note 不能图省事写成 (counts % FREQ) * (NSEC_PER_SEC / FREQ)——QEMU 上
 *   1e9/1e7 = 100 恰好整除、碰巧对，K210 的 390 MHz 不整除，那样写会系统性偏 20%。
 */
static inline uint64_t counts_to_ns(uint64_t counts)
{
    return (counts / TIMEBASE_FREQ_HZ) * NSEC_PER_SEC
         + (counts % TIMEBASE_FREQ_HZ) * NSEC_PER_SEC / TIMEBASE_FREQ_HZ;
}

/**
 * @brief 纳秒换算成 time CSR 计数值
 * @details 溢出规避同 counts_to_ns()，方向相反。
 */
static inline uint64_t ns_to_counts(uint64_t ns)
{
    return (ns / NSEC_PER_SEC) * TIMEBASE_FREQ_HZ
         + (ns % NSEC_PER_SEC) * TIMEBASE_FREQ_HZ / NSEC_PER_SEC;
}

void ktime_init(void);
/* 自内核启动以来的纳秒数，即 CLOCK_MONOTONIC。由 time CSR 直读换算而来，
 * 与中断状态、hart 无关；不要用 tick 计数代替（5 ms 分辨率、只统计 core 0）。 */
uint64_t ktime_get_ns(void);
/* CLOCK_REALTIME：ktime_get_ns() 加上一个可写的偏移量 */
uint64_t ktime_get_real_ns(void);
/* 设置墙钟：反算并改写偏移量。CLOCK_MONOTONIC 不受影响 */
void ktime_set_real_ns(uint64_t real_ns);

/* ---- ITIMER_REAL 进程定时器 ---- */

void ktime_alarm_init(void);
/**
 * @brief 装/改一个进程的 ITIMER_REAL 定时器
 * @param[in]  p           目标进程
 * @param[in]  expire_ns   首次到期的绝对时刻（ktime_get_ns() 时基）；0 表示取消
 * @param[in]  interval_ns 周期；0 表示单次
 * @param[out] old_value   非 NULL 时回填旧定时器的**剩余时间**与周期，均为纳秒
 * @param[out] old_interval 同上
 */
void ktime_alarm_set(struct proc_control_block *p, uint64_t expire_ns,
                     uint64_t interval_ns, uint64_t *old_value, uint64_t *old_interval);
/* 读一个进程的定时器剩余时间与周期（纳秒），未装时两者均为 0 */
void ktime_alarm_get(struct proc_control_block *p, uint64_t *value, uint64_t *interval);
/* 取消定时器并摘链；幂等，未装时是空操作 */
void ktime_alarm_cancel(struct proc_control_block *p);
/* 由 tick_int_handler() 每次 tick 调用：向所有到期的进程投 SIGALRM 并重排周期定时器 */
void ktime_check_alarms(void);

#endif
