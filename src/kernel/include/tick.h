#ifndef _TICK_H
#define _TICK_H

#include <stdint.h>

#include "sync.h"

extern osslock_t tick_lock;

/* time CSR 的计数频率。**必须与平台实际值一致**：此前长期沿用另一平台的折算值，
 * 实际 tick 周期变成约 0.195 秒（~5 Hz），因为在此之前没有任何功能依赖低延迟轮询，
 * 一直没被察觉，直到 TTY 的输入轮询接上去才暴露。
 *
 * 保持编译期常量而不是运行时变量，是因为它被 ktime.h 的内联函数与 syscall.c 当除数用，
 * 常量除法会被优化成乘加移位；改成变量就是每次 clock_gettime 都做一次真 64 位除法。 */
#if defined(QEMU)
#define TIMEBASE_FREQ_HZ 10000000UL     /* -machine dumpdtb 导出的 timebase-frequency 实测确认 */
#elif defined(VF2)
#define TIMEBASE_FREQ_HZ 4000000UL      /* JH7110，**待上板核对** */
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
void tick_int_handler(void);
/* 系统TICK是唯一的，既系统暴露给延时函数等的TICK值是唯一的，即核0上的tick计数值。 */
uint64_t tick_get_os_tick(void);
/* 多核都有独立的定时器中断和tick计数,tick_get_current()返回当前core的tick计数。 */
uint64_t tick_get_current(void);
void tick_set_os_tick(uint64_t tick);
/* 核忙等待延时，以tick为单位。 */
void tick_delay(uint64_t ticks);

#endif