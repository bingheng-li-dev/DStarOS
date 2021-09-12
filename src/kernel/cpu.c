#include "cpu.h"
#include "atomic.h"
#include "sbi.h"

extern volatile uint64_t core2Enabled;

extern uint64_t getCoreId_asm(void);
extern void setCoreId_asm(uint64_t coreMask);

static cpu_t CPUs[CORE_NUMBER];

void core2Enable(void)
{
    mb();
    unsigned long mask = 1 << 1; /* 使能core2:0x10 */
    sbi_send_ipi(&mask);
    core2Enabled = 0xa55a;
}

uint64_t getCoreId(void)
{
    return getCoreId_asm();
}

void setCoreId(uint64_t coreMask)
{
    return setCoreId_asm(coreMask & 0x1);
}

cpu_t *getCurrentCpu(void)
{
    return &CPUs[getCoreId()];
}

cpu_t *getSpecifiedCpu(uint16_t index)
{
    return &CPUs[index];
}

pcb_t *getCurrentProc(void)
{
    irqDisableNestingIncrement();
    cpu_t *cpu = getCurrentCpu();
    pcb_t *proc = cpu->currentProc;
    irqDisableNestingDecrement();
    return proc;
}
