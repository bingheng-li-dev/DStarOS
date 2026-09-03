#ifndef _TICK_H
#define _TICK_H

#include <stdint.h>

#include "sync.h"

extern osslock_t tick_lock;

/* time CSR 的计数频率，按平台实际主频折算：
 * QEMU virt 的 time CSR 为 10 MHz（-machine dumpdtb 导出的 timebase-frequency 实测确认）；
 * K210 无 time CSR，由 rustsbi-k210 模拟 CLINT mtime，频率对应其 390 MHz 主频折算。
 * 二者不可混用同一个常量——此前长期沿用 K210 的折算值在 QEMU 上运行，实际 tick 周期
 * 变成约 0.195 秒（~5 Hz），仅在此前从未依赖低延迟轮询的场景下未被察觉。
 * @todo 若日后硬件选型定为 VisionFive 2（JH7110），需按其 time CSR 实际频率
 *   （SoC 手册/设备树 timebase-frequency 实测值，不是 K210 的 390 MHz）新增一个
 *   平台分支，不可直接套用下面任一现有值。 */
#ifdef QEMU
#define TIMEBASE_FREQ_HZ 10000000UL
#else
#define TIMEBASE_FREQ_HZ 390000000UL
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
void tick_int_handler(void);
/* 系统TICK是唯一的，既系统暴露给延时函数等的TICK值是唯一的，即核0上的tick计数值。 */
uint64_t tick_get_os_tick(void);
/* 多核都有独立的定时器中断和tick计数,tick_get_current()返回当前core的tick计数。 */
uint64_t tick_get_current(void);
void tick_set_os_tick(uint64_t tick);
/* 核忙等待延时，以tick为单位。 */
void tick_delay(uint64_t ticks);

#endif