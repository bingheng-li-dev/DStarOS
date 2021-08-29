#include "tick.h"
#include "console.h"
#include "cpu.h"
#include "sbi.h"
#include "debug.h"
#include "encoding.h"

#define osTick getSpecifiedCpu(0)->tick

osslock_t ticksLock;
static uint64_t TIMEBASE = (390000000 / 200);

static inline uint64_t readtime(void)
{
    uint64_t x;
    asm volatile("csrr %0, time"
                 : "=r"(x));
    return x;
}

static void tickSetNextInt(uint64_t stime)
{
    sbi_set_timer(readtime() + stime);
#if DEBUG_TICK
    printf("++ setup timer interrupts\n");
#endif
}

/* 必须在trapInit()之后被调用 */
void tickInit(void)
{
    spinlockInit(&ticksLock);
    tickSetNextInt(TIMEBASE);
    getCurrentCpu()->tick = 0;
    printf("core %d tick inited!\n", getCoreId());
}

void tickIntHandler(void)
{
    spinlockAcquire(&ticksLock);
    getCurrentCpu()->tick += 1;
#if DEBUG_TICK
    if (getCurrentCpu()->tick % 100 == 0)
    {
        printf("core %ld : %ld ticks\n", getCurrentCpu()->tick);
    }
#endif
    //There seems sth to do on proc...
    spinlockRelease(&ticksLock);
    tickSetNextInt(TIMEBASE);
}

uint64_t getOSTick(void)
{
    return osTick;
}

uint64_t getCurrentTick(void)
{
    return getCurrentCpu()->tick;
}

void setOSTick(uint64_t tick)
{
    //interrupt_disable
    osTick = tick;
    //interrupt_enable
}

void delay(uint64_t ticks)
{
    uint64_t tick_start = getOSTick();
    uint64_t tick;
    do
    {
        tick = getOSTick();
        if (tick - tick_start == ticks)
            return;
    } while (1);
}
