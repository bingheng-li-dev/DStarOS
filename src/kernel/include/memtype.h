#ifndef _MEMTYPE_H_
#define _MEMTYPE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "encoding.h"

#define PGSHIFT RISCV_PGSHIFT
#define PGSIZE RISCV_PGSIZE
#define PTE_PPN_OFFSET 10
#define KERNEL_START 0x80200000
#define MEMORY_BASE DRAM_BASE
#define MEMORY_END 0x80800000

typedef uintptr_t phyAddr_t;
typedef uintptr_t virAddr_t;
typedef uint64_t ppn_t;
typedef uint64_t vpn_t;
typedef uint64_t pte_t;
typedef uint16_t pteflg_t;

static inline pte_t pteCreate(ppn_t ppn, pteflg_t pteFlag)
{
    return (pte_t)((ppn << PTE_PPN_OFFSET) | pteFlag | PTE_V);
}

static inline pteflg_t pteGetFlag(pte_t pte)
{
    return (pteflg_t)(pte & (pte_t)((1 << 8) - 0x1));
}

static inline ppn_t pteGetPpn(pte_t pte)
{
    return (ppn_t)((pte >> PTE_PPN_OFFSET) & (pte_t)(((pte_t)1 << 44) - 0x1));
}

/* Return true if(PTE_V & pte).  */
static inline bool pteIsValid(pte_t pte)
{
    return (pte & PTE_V) != 0x0;
}

static inline bool pteReadable(pte_t pte)
{
    return (pte & PTE_R) != 0x0;
}

static inline bool pteWritable(pte_t pte)
{
    return (pte & PTE_W) != 0x0;
}

static inline bool pteExecutable(pte_t pte)
{
    return (pte & PTE_X) != 0x0;
}

static inline void dropTLB(void)
{
    asm volatile("sfence.vma");
}

static inline void refreshTLB(virAddr_t va)
{
    asm volatile("sfence.vma %0"
                 :
                 : "r"(va));
}

#endif