#include "tick.h"
#include "console.h"
#include "sync.h"
#include "sbi.h"
#include "debug.h"
#include "encoding.h"
#include "sched.h"
#include "tty.h"
#include "ktime.h"

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
