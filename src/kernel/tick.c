#include "tick.h"
#include "console.h"
#include "sync.h"
#include "sbi.h"
#include "debug.h"
#include "encoding.h"
#include "sched.h"

#define OS_TICK cpu_get_by_index(0)->tick

osslock_t tick_lock;
static uint64_t timebase = (390000000 / 200);

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
    spinlock_acquire(&tick_lock);
    cpu_get_current()->tick += 1;
#if DEBUG_TICK
    if (cpu_get_current()->tick % 100 == 0)
    {
        printf("core %ld : %ld ticks\n", cpu_get_current()->tick);
    }
#endif
    sched_task_tick();
    spinlock_release(&tick_lock);
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
