#include "tick.h"

static uint64_t TIMEBASE = 100000;

#if DEBUG_TICK
volatile uint64_t tick;
#endif

void tick_set_next_int(uint64_t stime)
{
    sbi_set_timer(readtime() + stime);
#if DEBUG_TICK
    printf("++ setup timer interrupts\n");
#endif
}

void tick_init(void)
{
    tick_set_next_int(TIMEBASE);
#if DEBUG_TICK
    tick = 0;
    printf("tick inited!\n");
#endif
    set_csr(sie, MIP_STIP);
    printf("tick inited!\n");
}

void tick_int_handler(void)
{
#if DEBUG_TICK
    if (++tick % 100 == 0)
    {
        printf("%ld ticks\n", tick);
    }
#endif
    tick_set_next_int(TIMEBASE);
}
