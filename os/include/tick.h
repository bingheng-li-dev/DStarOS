#ifndef _TICK_H
#define _TICK_H

#include <stdint.h>

#include "debug.h"
#include "sbi.h"
#include "tinyprintf.h"
#include "encoding.h"

void tick_init(void);
void tick_int_handler(void);

static inline uint64_t readtime(void)
{
    uint64_t x;
    asm volatile("csrr %0, time"
                 : "=r"(x));
    return x;
}

#endif