#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "proc.h"
#include "sync.h"

/* 1 <= CORE_NUMBER. */
#define CORE_NUMBER 2

typedef struct os_cpu cpu_t;

struct os_cpu
{
    pcb_t *currentProc;
    ctx_t *cxt;
    int irqDisableNesting; /* 中断请求屏蔽嵌套数量。 */
    bool intrDisableState; /* 当前中断屏蔽开启/关闭状态(true/false)。eg:如果中断处于关闭状态时"intrDisableState"为true。 */
    uint64_t tick;         /* 当前CPU的tick。 */
};

void core2Enable(void);
uint64_t getCoreId(void);
void setCoreId(uint64_t coreMask);
cpu_t *getCurrentCpu(void);
cpu_t *getSpecifiedCpu(uint16_t index);
pcb_t *getCurrentProc(void);

#endif