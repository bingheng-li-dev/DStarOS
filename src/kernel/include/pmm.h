/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _PMM_H
#define _PMM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "encoding.h"
#include "list.h"
#include "debug.h"

#include "memtype.h"

typedef struct phy_frame pframe_t;
typedef struct free_space_list fslist_t;
typedef struct bestfit_frame_allocator bffa_t;
typedef struct kmem_cache kmem_cache_t;

/* PMM 记页数的类型。**它的宽度直接决定能管理多少物理内存**——页数一旦溢出就静默回绕，
 * pmm_page_list 数组按回绕后的小值分配，此后对高位页帧的每一次访问都是越界写，
 * 没有任何护栏接得住。这条约束原先散落在各处的 uint16_t 里（65535 页 = 256 MB）、
 * 谁都看不见，现在收拢到这一个 typedef 上：要改上限只需要动这一行，
 * 下面的 _Static_assert 会跟着自动更新。
 * 注意 pframe_t.reference 与 slab_inuse **不是**页数，不要一起改。 */
typedef uint32_t pgcount_t;

struct phy_frame
{
    pgcount_t nsize;    /* The size of this block which free or to be used. */
    uint16_t reference; /* Amount of vir page used. */
    bool can_be_alloc;    /* True:this frame is the head of a free block and can be allocated;false:this frame is in usage or it is not the head of a block. */
    struct list_head free_list_linker;
    struct list_head free_addr_list_linker;
    kmem_cache_t *slab_cache;      /* NULL 表示非 slab 页，kfree 靠它 O(1) 分派 */
    void *slab_freelist;           /* 页内第一个空闲对象（KVA） */
    uint16_t slab_inuse;           /* 页内已分配对象数 */
    struct list_head slab_linker;  /* 挂进 cache->partial[] / cache->full */
};

struct free_space_list /* Record the addr of the free list from small to large. */
{
    struct list_head list_linker;
    pgcount_t fnsize; /* PGSIZE times */
};

struct bestfit_frame_allocator /* Best fit,it allows to allocate a continuous block of memory. */
{
    /* Used for allocation,insert alloced remaining mems into free list.Returns the addr of alloced mems. */
    pframe_t *(*bffa_delete_and_reinsert)(pgcount_t nsize);
    /* Insert and merge dealloced mems base addr into free list. */
    void (*bffa_insert_and_merge)(pframe_t *base_frame, pgcount_t nsize);
};

/* 可管理的最大页数，直接由 pgcount_t 的宽度推导。
 * 把它钉成编译期断言，是为了让"物理内存上限"从藏在类型里的暗雷变成会报错的约束：
 * 谁把 MEMORY_END 调过头，编译当场失败，而不是运行期静默越界写。 */
#define PMM_MAX_PAGES ((uint64_t)(pgcount_t) ~(pgcount_t)0)

_Static_assert((MEMORY_END - KERNEL_START) / PGSIZE <= PMM_MAX_PAGES,
               "MEMORY_END too large: page count overflows pgcount_t (see pmm.h)");

/* Each pframe maps a ppn/pa,use convert_pframe2ppn/pa to covert. */
extern pframe_t *pmm_page_list;
extern fslist_t pmm_free_list;
extern fslist_t pmm_free_addr_list;

void pmm_init(void);
pframe_t *pmm_alloc_pages(pgcount_t nsize);
pframe_t *pmm_alloc_page(void);
void pmm_free_pages(pframe_t *base_frame);
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

static inline ppn_t convert_pframe2ppn(pframe_t *current_frame)
{
    /* KERNEL_START使用绝对地址，因为开启MMU后变为高位虚拟地址 */
    return (current_frame - pmm_page_list + convert_pa2ppn_flr(KERNEL_START));
}

static inline phyAddr_t convert_ppn2pa(ppn_t ppn)
{
    return (ppn * PGSIZE);
}

static inline phyAddr_t convert_pframe2pa(pframe_t *current_frame)
{
    return (convert_pframe2ppn(current_frame) * PGSIZE);
}

/* 返回 pframe 对应物理页的内核虚拟地址，用于 MMU 开启后读写页面内容 */
static inline virAddr_t convert_pframe2kva(pframe_t *current_frame)
{
    return pa_to_kva(convert_pframe2pa(current_frame));
}

static inline pframe_t *convert_ppn2pframe(ppn_t ppn)
{
    return (ppn - convert_pa2ppn_flr(KERNEL_START) + pmm_page_list);
}

static inline pframe_t *convert_pa2pframe_flr(phyAddr_t pa)
{
    return convert_ppn2pframe(convert_pa2ppn_flr(pa));
}

#endif
