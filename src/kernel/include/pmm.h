#ifndef _PMM_H
#define _PMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "encoding.h"
#include "list.h"
#include "debug.h"

#include "memtype.h"

typedef struct phyframe pframe_t;
typedef struct freeSpaceList fslist_t;
typedef struct bestfitFrameAllocator bffa_t;
typedef struct kmem_cache kmem_cache_t;

struct phyframe
{
    uint16_t nsize;     /* The size of this block which free or to be used. */
    uint16_t reference; /* Amount of vir page used. */
    bool canBeAlloc;    /* True:this frame is the head of a free block and can be alloc;false:this frame is in usage or it is not the head of a block. */
    virAddr_t va;       /* Used for pra. */
    struct list_head list_linker_inFreeList;
    struct list_head list_linker_inFreeAList;
    struct list_head list_linker_inClockList;
    kmem_cache_t *slab_cache;      /* NULL 表示非 slab 页，kfree 靠它 O(1) 分派 */
    void *slab_freelist;           /* 页内第一个空闲对象（KVA） */
    uint16_t slab_inuse;           /* 页内已分配对象数 */
    struct list_head slab_linker;  /* 挂进 cache->partial[] / cache->full */
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

/* PMM 全程用 uint16_t 记页数（pframe_t.nsize、fslist_t.fnsize、alloc() 的形参、
 * pmm_init 里的 cursor），**这个宽度直接决定了能管理多少物理内存**：
 * 页数一旦超过 65535 就静默回绕，PageListBegin 数组按回绕后的小值分配，
 * 此后对高位页帧的每一次访问都是越界写，且不会有任何护栏接住。
 * 这条约束此前只隐含在类型里，谁都看不见，所以在这里钉成编译期断言。 */
#define PMM_MAX_PAGES ((uint64_t)UINT16_MAX)

_Static_assert((MEMORY_END - KERNEL_START) / PGSIZE <= PMM_MAX_PAGES,
               "MEMORY_END too large: page count overflows the uint16_t counters in pmm.c");

/* Each pframe maps a ppn/pa,use convert_pframe2ppn/pa to covert. */
extern pframe_t *PageListBegin;
extern fslist_t FreeList;
extern fslist_t FreeAList;

void pmm_init(void);
pframe_t *alloc(uint16_t nsize);
pframe_t *alloc_page(void);
void dealloc(pframe_t *baseppn);
void pmm_init_after_mmu_enable(void);

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
    /* KERNEL_START使用绝对地址，因为开启MMU后变为高位虚拟地址 */
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

/* 返回 pframe 对应物理页的内核虚拟地址，用于 MMU 开启后读写页面内容 */
static inline virAddr_t convert_pframe2kva(pframe_t *currentFrame)
{
    return pa_to_kva(convert_pframe2pa(currentFrame));
}

static inline pframe_t *convert_ppn2pframe(ppn_t ppn)
{
    return (ppn - convert_pa2ppn_flr(KERNEL_START) + PageListBegin);
}

static inline pframe_t *convert_pa2pframe_flr(phyAddr_t pa)
{
    return convert_ppn2pframe(convert_pa2ppn_flr(pa));
}

#endif
