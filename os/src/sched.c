#include "sched.h"
#include "trap.h"
#include "proc.h"

void sched(void)
{
    irq_disable();
    
    irq_enable();
}

void schedStart(void)
{
    
}