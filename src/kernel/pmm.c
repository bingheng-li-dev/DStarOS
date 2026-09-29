/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "pmm.h"
#include "stringops.h"
#include "console.h"
#include "sync.h"
#include "memtype.h"
#include "startup.h"

pframe_t *pmm_page_list;
fslist_t pmm_free_list;  /* Free memories will arrange from small size to large size,used for best fit. */
fslist_t pmm_free_addr_list; /* Free memories will arrange from small addr to large addr,used for merge. */
osslock_t pmm_lock;

static pframe_t *delete_and_reinsert(pgcount_t nsize);
static void insert_and_merge(pframe_t *base_frame, pgcount_t nsize);

const bffa_t BFallocator = {
    .bffa_delete_and_reinsert = delete_and_reinsert,
    .bffa_insert_and_merge = insert_and_merge,
};

/**
 * @brief 建立页帧元数据数组与两条空闲链表
 * @note 在 MMU 开启前调用；此时存下的全是物理地址，开启后由 pmm_init_after_mmu_enable() 修正。
 */
void pmm_init(void)
{
    spinlock_init(&pmm_lock);

    phyAddr_t kernel_end_addr   = (phyAddr_t)ekernel;
    phyAddr_t kernel_start_addr = (phyAddr_t)skernel;
    phyAddr_t kernel_entry_addr = (phyAddr_t)_start;
    printf("kernel: skernel:0x%08lx, ekernel:0x%08lx, kernel_entry:0x%08lx\n", kernel_start_addr, kernel_end_addr, kernel_entry_addr);

    phyAddr_t kernel_roundup_end_addr = pa_roundup(kernel_end_addr);
    pmm_page_list = (pframe_t *)kernel_roundup_end_addr;

    ppn_t ppn_begin = convert_pa2ppn_cil(kernel_start_addr);
    ppn_t ppn_end = convert_pa2ppn_cil(MEMORY_END);
    pgcount_t ppn_total_amount = ppn_end - ppn_begin; /* Total amount of the ppn.*/

    /* 内核镜像之后、空闲内存之前的这段用来存页帧元数据数组 */
    uint64_t page_list_bytes = sizeof(pframe_t) * ppn_total_amount;

    phyAddr_t free_memory_begin_addr = (phyAddr_t)pmm_page_list + (phyAddr_t)page_list_bytes;

    uint64_t free_memory_size = (uint64_t)(MEMORY_END - free_memory_begin_addr);
    ppn_t ppn_free_begin = convert_pa2ppn_cil(free_memory_begin_addr);
    ppn_t ppn_free_end = ppn_end;

    printf("physical memory map:\n");
    printf("memory: 0x%08lx, [0x%08lx, 0x%08x].\n", free_memory_size, free_memory_begin_addr, MEMORY_END);
    printf("ppn: ppn_total_amount:%08u, [ppn_free_begin:%08ld, ppn_free_end:%08ld].\n", ppn_total_amount, ppn_free_begin, ppn_free_end);

    pgcount_t cursor;
    for (cursor = 0; cursor < ppn_free_begin - ppn_begin; cursor++)
    {
        pmm_page_list[cursor].can_be_alloc = 0;
        pmm_page_list[cursor].slab_cache = NULL;
    }

    pframe_t *free_frame_begin = &(pmm_page_list[cursor]);
    for (; cursor < ppn_total_amount; cursor++)
    {
        pmm_page_list[cursor].can_be_alloc = 1;
        pmm_page_list[cursor].reference = 0;
        /* kfree 完全依赖这个字段判定页类型，残留垃圾值会让第一次 kfree 非 slab 页就跳错分支 */
        pmm_page_list[cursor].slab_cache = NULL;
    }

    /* ppn_free_end 是开区间上界，这里不能 +1：多出的那一页 PA 恰好等于 MEMORY_END，
     * 既不在 pmm_page_list 数组里，也没有 KVA 映射。它一旦被当正常页分配出去，
     * 写它的人就会撞上"找不到 VMA"的 segfault。 */
    pgcount_t ppn_free_amount = ppn_free_end - ppn_free_begin;
    free_frame_begin->nsize = ppn_free_amount;

    INIT_LIST_HEAD((&pmm_free_list.list_linker));
    pmm_free_list.fnsize = ppn_free_amount;
    list_add(&(free_frame_begin->free_list_linker), &(pmm_free_list.list_linker));

    INIT_LIST_HEAD((&pmm_free_addr_list.list_linker));
    pmm_free_addr_list.fnsize = ppn_free_amount;
    list_add(&(free_frame_begin->free_addr_list_linker), &(pmm_free_addr_list.list_linker));

    printf("pmm: inited\n");
}

/**
 * @brief 把 pmm_init() 里存下的物理地址修正为内核虚拟地址
 * @details pmm_page_list、pmm_free_list、pmm_free_addr_list 存的都是物理地址，
 *   MMU 开启后这些值会被当成虚拟地址解释，一访问就是不该有的缺页。
 */
void pmm_init_after_mmu_enable(void)
{
    /* pmm_page_list 先前存的是绝对物理地址，换成虚拟地址 */
    pmm_page_list = (pframe_t *)pa_to_kva((phyAddr_t)pmm_page_list);

    /* 两条空闲链表里的指针，连同 dummy head 本身，也要一起更新 */
    pmm_free_list.list_linker.next = (struct list_head *)pa_to_kva(
        (phyAddr_t)pmm_free_list.list_linker.next);
    pmm_free_list.list_linker.prev = (struct list_head *)pa_to_kva(
        (phyAddr_t)pmm_free_list.list_linker.prev);
    struct list_head *pos;
    list_for_each(pos, &pmm_free_list.list_linker)
    {
        pos->next = (struct list_head *)pa_to_kva((phyAddr_t)pos->next);
        pos->prev = (struct list_head *)pa_to_kva((phyAddr_t)pos->prev);
    }
    pmm_free_addr_list.list_linker.next = (struct list_head *)pa_to_kva(
        (phyAddr_t)pmm_free_addr_list.list_linker.next);
    pmm_free_addr_list.list_linker.prev = (struct list_head *)pa_to_kva(
        (phyAddr_t)pmm_free_addr_list.list_linker.prev);
    list_for_each(pos, &pmm_free_addr_list.list_linker)
    {
        pos->next = (struct list_head *)pa_to_kva((phyAddr_t)pos->next);
        pos->prev = (struct list_head *)pa_to_kva((phyAddr_t)pos->prev);
    }
}

/**
 * @brief 分配 nsize 个连续物理页
 * @param[in] nsize 需要的页数
 * @retval NULL 空闲总量不足，或总量够但没有足够长的连续块（外部碎片）
 * @return 首页的 pframe_t
 * @note 失败一律返回 NULL，绝不在这里做回收重试——本函数持有 pmm_lock，
 *   而 slab_reclaim_all() 的锁序是 cache->lock → pmm_lock，就地调用会 ABBA 死锁
 *   并二次 acquire 非重入锁。重试由 kmalloc()/缺页处理等不持锁的调用层负责。
 */
pframe_t *pmm_alloc_pages(pgcount_t nsize)
{
    pframe_t *ret = NULL;
    irq_key_t PmmLock_key = spinlock_acquire(&pmm_lock);
    if (nsize > pmm_free_list.fnsize)
    {
        goto f1;
    }
    ret = BFallocator.bffa_delete_and_reinsert(nsize);

    if (ret != NULL)
    {
        pframe_t *current_frame;
        for (current_frame = ret; current_frame != ret + nsize; current_frame++)
        {
            current_frame->can_be_alloc = 0;
        }

        /* MMU 关闭时 PA 可直接解引用；开启后物理地址无恒等映射，需经 KVA */
        phyAddr_t frame_pa = convert_pframe2pa(ret);
        void *zero_dst = mmu_is_enabled()
                         ? (void *)pa_to_kva(frame_pa)
                         : (void *)frame_pa;
        memset(zero_dst, 0, (size_t)nsize * PGSIZE);

        ret->nsize = nsize;
    }

f1:
    spinlock_release(&pmm_lock, PmmLock_key);
    return ret;
}

/**
 * @brief 分配一个物理页
 */
pframe_t *pmm_alloc_page(void)
{
    return pmm_alloc_pages((pgcount_t)1);
}

/**
 * @brief 归还一个由 pmm_alloc_pages() 分配的块，块大小取自 base_frame->nsize
 */
void pmm_free_pages(pframe_t *base_frame)
{
    irq_key_t PmmLock_key = spinlock_acquire(&pmm_lock);

    pframe_t *current_frame;
    pgcount_t nsize;
    nsize = base_frame->nsize;
    for (current_frame = base_frame; current_frame != base_frame + nsize; current_frame++)
    {
        current_frame->can_be_alloc = 1;
        current_frame->reference = 0;
        /* 不清的话这页被 pmm_alloc_pages() 分配成普通页后，kfree 会照着残留的 cache 指针
         * 把它当 slab 页处理 */
        current_frame->slab_cache = NULL;
    }

    BFallocator.bffa_insert_and_merge(base_frame, nsize);

    spinlock_release(&pmm_lock, PmmLock_key);
}

/* 按块大小升序插进 pmm_free_list：排在第一个不小于它的块之前，都比它小就放末尾 */
static void free_list_insert_by_size(pframe_t *frame)
{
    struct list_head *pos;
    list_for_each(pos, &(pmm_free_list.list_linker))
    {
        if (list_entry(pos, pframe_t, free_list_linker)->nsize >= frame->nsize)
        {
            list_add_tail(&(frame->free_list_linker), pos);
            return;
        }
    }
    list_add_tail(&(frame->free_list_linker), &(pmm_free_list.list_linker));
}

/* 按地址升序插进 pmm_free_addr_list */
static void free_addr_list_insert(pframe_t *frame)
{
    struct list_head *pos;
    list_for_each(pos, &(pmm_free_addr_list.list_linker))
    {
        if (list_entry(pos, pframe_t, free_addr_list_linker) > frame)
        {
            list_add_tail(&(frame->free_addr_list_linker), pos);
            return;
        }
    }
    list_add_tail(&(frame->free_addr_list_linker), &(pmm_free_addr_list.list_linker));
}

static pframe_t *delete_and_reinsert(pgcount_t nsize)
{
    pframe_t *ret = NULL, *current_frame;
    struct list_head *current_entry;
    list_for_each(current_entry, &(pmm_free_list.list_linker))
    {
        current_frame = list_entry(current_entry, pframe_t, free_list_linker);
        if (current_frame->nsize >= nsize)
        {
            ret = current_frame;
            break;
        }
    }
    if (ret != NULL)
    {
        struct list_head *addr_prev = ret->free_addr_list_linker.prev;
        list_del(&(ret->free_list_linker));
        list_del(&(ret->free_addr_list_linker));
        if (ret->nsize > nsize)
        {
            /* 剩下的后半块在地址链表里接替原块的位置，地址顺序不变 */
            pframe_t *reinsert_frame = ret + nsize;
            reinsert_frame->nsize = ret->nsize - nsize;
            list_add(&(reinsert_frame->free_addr_list_linker), addr_prev);
            free_list_insert_by_size(reinsert_frame);
        }
        pmm_free_list.fnsize = pmm_free_list.fnsize - nsize;
        pmm_free_addr_list.fnsize = pmm_free_addr_list.fnsize - nsize;
    }
    return ret;
}

/* 刚插进地址链表的 base_frame 与地址上紧邻的前后空闲块合并，返回合并后的块首。
 * 被并掉的块从两条链表里摘下；返回的块此时不在 pmm_free_list 里，由调用方插回。 */
static pframe_t *merge_neighbors(pframe_t *base_frame)
{
    pframe_t *prev_frame_in_addr_list, *next_frame_in_addr_list;
    if (pmm_free_addr_list.list_linker.next == &(base_frame->free_addr_list_linker))
    {
        prev_frame_in_addr_list = NULL;
    }
    else
    {
        prev_frame_in_addr_list = list_entry((base_frame->free_addr_list_linker).prev, pframe_t, free_addr_list_linker);
    }
    if (pmm_free_addr_list.list_linker.prev == &(base_frame->free_addr_list_linker))
    {
        next_frame_in_addr_list = NULL;
    }
    else
    {
        next_frame_in_addr_list = list_entry((base_frame->free_addr_list_linker).next, pframe_t, free_addr_list_linker);
    }
    pframe_t *merged_frame = base_frame;

    /* 先与后面的合并，因为可能存在需要同时合并前面和后面的情况。 */
    if (next_frame_in_addr_list != NULL && base_frame + base_frame->nsize == next_frame_in_addr_list)
    {
        list_del(&(next_frame_in_addr_list->free_addr_list_linker));
        list_del(&(next_frame_in_addr_list->free_list_linker));
        base_frame->nsize = base_frame->nsize + next_frame_in_addr_list->nsize;
    }
    if (prev_frame_in_addr_list != NULL &&
        prev_frame_in_addr_list + prev_frame_in_addr_list->nsize == base_frame)
    {
        list_del(&(base_frame->free_addr_list_linker));
        list_del(&(prev_frame_in_addr_list->free_list_linker));
        prev_frame_in_addr_list->nsize = prev_frame_in_addr_list->nsize + base_frame->nsize;
        merged_frame = prev_frame_in_addr_list;
    }
    return merged_frame;
}

/**
 * @brief 把回收的块插回空闲链表，并与地址相邻的空闲块合并
 * @param[in] base_frame 被回收块的首个页帧
 * @param[in] nsize      被回收块的页数
 */
static void insert_and_merge(pframe_t *base_frame, pgcount_t nsize)
{
    /* 顺序固定：先插进按地址排的链表，按需合并，最后才插进按大小排的链表。
     * 合并后 base_frame->nsize 已是总大小，计数只能加参数 nsize。 */
    pmm_free_addr_list.fnsize = pmm_free_addr_list.fnsize + nsize;
    free_addr_list_insert(base_frame);
    pframe_t *merged_frame = merge_neighbors(base_frame);
    pmm_free_list.fnsize = pmm_free_list.fnsize + nsize;
    free_list_insert_by_size(merged_frame);
}
