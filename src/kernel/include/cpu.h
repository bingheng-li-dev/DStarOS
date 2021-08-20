#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "atomic.h"
#include "proc.h"

/* 1 <= CORE_NUMBER. */
#define CORE_NUMBER 2

typedef struct os_spin_lock osslock_t;
typedef struct os_cpu   cpu_t;

struct os_spin_lock
{
    spinlock_t spinlock;
};

struct os_cpu
{
    pcb_t *currentProc;


    uint64_t tick;
};


void core2Enable(void);
uint64_t getCoreId(void);
void setCoreId(uint64_t coreMask);
void spinlockInit(osslock_t *lock);
void spinlockAcquire(osslock_t *lock);
void spinlockRelease(osslock_t *lock);
cpu_t* getCurrentCpu(void);
cpu_t* getSpecifiedCpu(uint16_t index);


#endif