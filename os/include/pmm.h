#ifndef _PMM_H
#define _PMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "encoding.h"
#include "list.h"
#include "debug.h"
#include "tinyprintf.h"

#define PGSHIFT RISCV_PGSHIFT
#define PGSIZE RISCV_PGSIZE
#define PTE_PPN_OFFSET 10
#define KERNEL_START 0x80200000
#define MEMORY_BASE DRAM_BASE
#define MEMORY_END 0x80800000

#define SATPMODE_BARE 0x0000000000000000
#define SATPMODE_RV39 0x8000000000000000

typedef uintptr_t phyAddr_t;
typedef uintptr_t virAddr_t;
typedef uint64_t ppn_t;
typedef uint64_t vpn_t;
typedef uint64_t pte_t;
typedef uint16_t pteflg_t;

typedef struct phyframe pframe_t;
typedef struct freeSpaceList fslist_t;
typedef struct bestfitFrameAllocator bffa_t;

struct phyframe
{
    uint16_t nsize;     /* The size of this block which free or to be used. */
    uint16_t reference; /* Amount of vir page used. */
    bool canBeAlloc;    /* True:this frame is the head of a free block and can be alloc;false:this frame is in usage or it is not the head of a block. */
    virAddr_t va;       /* Used for pra. */
    struct list_head list_linker_inFreeList;
    struct list_head list_linker_inFreeAList;
    struct list_head list_linker_inClockList;
};

struct freeSpaceList /* Record the addr of the free list from small to large. */
{
    struct list_head list_linker;
    uint16_t fnsize; /* PGSIZE times */
};

struct bestfitFrameAllocator /* Best fit,it allows to allocate a continuous block of memory. */
{
    /* Used for alloc,insert alloced remaining mems into free list.Returns the addr of alloced mems. */
    pframe_t *(*bffa_deleteAndReinsert)(uint16_t nsize);
    /* Insert and merge dealloced mems base addr into free list. */
    void (*bffa_insertAndMerge)(pframe_t *baseppn, uint16_t nsize);
};

/* Each pframe maps a ppn/pa,use convert_pframe2ppn/pa to covert. */
extern pframe_t *PageListBegin;
extern fslist_t FreeList;
extern fslist_t FreeAList;

void pmm_init(void);
void *pmm_alloc(uint16_t nsize);
void *pmm_allocOneFrame(void);
pte_t *pmm_pteGet(pframe_t *pageTable, virAddr_t va);
void pmm_pteRemove(virAddr_t va, pte_t *pte);
pte_t *pmm_pteInsert(pframe_t *pageTable, virAddr_t va, pteflg_t pteFlag);

void *kmalloc(uint64_t size);
void kfree(void *ptr);

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

/* RoundDown(Left)*/
static inline ppn_t convert_pa2ppn_flr(phyAddr_t pa)
{
    return (ppn_t)((uintptr_t)pa / PGSIZE);
}

/* RoundUp(Right) */
static inline ppn_t convert_pa2ppn_cil(phyAddr_t pa)
{
    return (ppn_t)(((uintptr_t)pa + PGSIZE - 1) / PGSIZE);
}

/* RoundUp */
static inline phyAddr_t pa_roundup(phyAddr_t pa)
{
    return (pa + PGSIZE - (pa % PGSIZE));
}

static inline ppn_t convert_pframe2ppn(pframe_t *currentFrame)
{
    return (currentFrame - PageListBegin + convert_pa2ppn_flr(KERNEL_START));
}

static inline phyAddr_t convert_ppn2pa(ppn_t ppn)
{
    return (ppn * PGSIZE);
}

static inline phyAddr_t convert_pframe2pa(pframe_t *currentFrame)
{
    return (convert_pframe2ppn(currentFrame) * PGSIZE);
}

/* @param level must equal 3, 2 or 1. (left -> right) */
static inline vpn_t convert_va2vpn(virAddr_t va, uint16_t level)
{
    return ((va >> ((level - 1) * 9 + PGSHIFT)) & 0x1FF);
}

static inline pframe_t *convert_ppn2pframe(ppn_t ppn)
{
    return (ppn - convert_pa2ppn_flr(KERNEL_START) + PageListBegin);
}

static inline pframe_t *convert_pte2pframe(pte_t pte)
{
    return (convert_ppn2pframe(pteGetPpn(pte)));
}

static inline pframe_t *convert_pa2pframe_flr(phyAddr_t pa)
{
    return convert_ppn2pframe(convert_pa2ppn_flr(pa));
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
