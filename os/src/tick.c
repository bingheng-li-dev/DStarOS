#include "tick.h"
#include "debug.h"
#include "sbi.h"
#include "tinyprintf.h"
#include "encoding.h"

static uint64_t TIMEBASE = 100000;

#if DEBUG
volatile uint64_t tick;
#endif

void tick_set_next_int(uint64_t stime)
{
    sbi_set_timer(readtime() + stime);
#if DEBUG
    printf("++ setup timer interrupts\n");
#endif
}

void tick_init(void)
{
    tick_set_next_int(TIMEBASE);
#if DEBUG
    tick = 0;
    printf("tick inited!\n");
#endif
    set_csr(sie, MIP_STIP);
}

void tick_int_handler(void)
{
#ifdef DEBUG
    if (++tick % 100 == 0)
    {
        printf("%ld ticks\n", tick);
    }
#endif
    tick_set_next_int(TIMEBASE);
}