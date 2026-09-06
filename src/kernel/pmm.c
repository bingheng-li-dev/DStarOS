#include "pmm.h"
#include "stringops.h"
#include "console.h"
#include "sync.h"
#include "memtype.h"

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

void pmm_init(void)
{
    spinlock_init(&pmm_lock);

    extern char _start[];
    phyAddr_t kernel_end_addr   = (phyAddr_t)ekernel;
    phyAddr_t kernel_start_addr = (phyAddr_t)skernel;
    phyAddr_t kernel_entry_addr = (phyAddr_t)_start;
    printf("kernel: skernel:0x%08lx, ekernel:0x%08lx, kernel_entry:0x%08lx\n", kernel_start_addr, kernel_end_addr, kernel_entry_addr);

    phyAddr_t kernel_roundup_end_addr = pa_roundup(kernel_end_addr);
    /* Page list begin addr. */
    pmm_page_list = (pframe_t *)kernel_roundup_end_addr;

    ppn_t ppn_begin = convert_pa2ppn_cil(kernel_start_addr);
    ppn_t ppn_end = convert_pa2ppn_cil(MEMORY_END);
    pgcount_t ppn_total_amount = ppn_end - ppn_begin; /* Total amount of the ppn.*/

    /* This section of memory between end of roundup kernel and the begin of free memory is used for store page list.*/
    uint64_t page_list_bytes = sizeof(pframe_t) * ppn_total_amount;

    /* True end of kernel used memory. */
    phyAddr_t free_memory_begin_addr = (phyAddr_t)pmm_page_list + (phyAddr_t)page_list_bytes;

    uint64_t free_memory_size = (uint64_t)(MEMORY_END - free_memory_begin_addr);
    ppn_t ppn_free_begin = convert_pa2ppn_cil(free_memory_begin_addr);
    ppn_t ppn_free_end = ppn_end;

    printf("physical memory map:\n");
    printf("memory: 0x%08lx, [0x%08lx, 0x%08x].\n", free_memory_size, free_memory_begin_addr, MEMORY_END);
    printf("ppn: ppn_total_amount:%08u, [ppn_free_begin:%08ld, ppn_free_end:%08ld].\n", ppn_total_amount, ppn_free_begin, ppn_free_end);

    /* Set status of kernel memory and free memory for pages. */
    pgcount_t cursor;
    for (cursor = 0; cursor < ppn_free_begin - ppn_begin; cursor++)
    {
        pmm_page_list[cursor].can_be_alloc = 0;
        pmm_page_list[cursor].slab_cache = NULL;
    }

    /* Initial the memory map and free lists. */
    pframe_t *free_frame_begin = &(pmm_page_list[cursor]);
    for (; cursor < ppn_total_amount; cursor++)
    {
        pmm_page_list[cursor].can_be_alloc = 1;
        pmm_page_list[cursor].reference = 0;
        /* kfree 完全依赖这个字段判定页类型，残留垃圾值会让第一次 kfree 非 slab 页就跳错分支 */
        pmm_page_list[cursor].slab_cache = NULL;
    }

    /* ppn_free_end（=ppn_end）和 ppn_total_amount 一样是开区间上界（不含），这里不能 +1——
     * 加了会让空闲块的登记大小比 pmm_page_list 数组和 init_kernel_offset_mapping()
     * 实际映射的范围都多出一页，那一页的 PA 恰好等于 MEMORY_END，对应的 KVA
     * 从未被建立映射；pmm_alloc_pages() 迟早会把这个幻影页当正常页分配出去，谁写它谁触发
     * "va=KVA(MEMORY_END) 找不到 VMA" 的 segfault——纯物理内存分配量小、命中概率低时
     * 不容易撞见，分配压力上来后（比如两个 hart 真并发分配）就容易复现。 */
    pgcount_t ppn_free_amount = ppn_free_end - ppn_free_begin;
    free_frame_begin->nsize = ppn_free_amount;

    INIT_LIST_HEAD((&pmm_free_list.list_linker));
    pmm_free_list.fnsize = ppn_free_amount;
    list_add(&(free_frame_begin->free_list_linker), &(pmm_free_list.list_linker));

    INIT_LIST_HEAD((&pmm_free_addr_list.list_linker));
    pmm_free_addr_list.fnsize = ppn_free_amount;
    list_add(&(free_frame_begin->free_addr_list_linker), &(pmm_free_addr_list.list_linker));

#if DEBUG_MMU_mm_init
    printf("ppn_free_amount:%u\n", ppn_free_amount);
    printf("pmm_free_list.fnsize:%u, pmm_free_addr_list.fnsize:%u\n", pmm_free_list.fnsize, pmm_free_addr_list.fnsize);
#endif

    printf("pmm inited!\n");
}

/**
 * @name pmm_init_after_mmu_enable
 * @brief 将MMU开启前pmm初始化时相关变量存储的物理地址修复为虚拟地址
 * @details 之前FreeList、pmm_free_addr_list、PageListBegin这些指针变量
 * 都存储了物理地址，在开启MMU后会导致MMU将这些物理地址作为虚拟地址使用触发不应该的
 * 缺页异常，需要在MMU开启后进行修复
 */
void pmm_init_after_mmu_enable(void)
{
    /* 先前pmm_init中PageListBegin指针存储了绝对物理地址，换成虚拟地址 */
    pmm_page_list = (pframe_t *)pa_to_kva((phyAddr_t)pmm_page_list);

    /* FreeList和FreeAList中的指针包括dummy head本身也需要更新 */
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
 * @name pmm_alloc_pages
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

        /* "ret->nsize" restores the size of this alloced block which is convenient to free block. */
        ret->nsize = nsize;

#if DEBUG_MMU_mm_alloc
        printf("pmm_alloc_pages::Frame has been allocated!ppn:%ld,pa:%08lx\n", convert_pframe2ppn(ret), convert_pframe2pa(ret));
#endif
    }

f1:
    spinlock_release(&pmm_lock, PmmLock_key);
    return ret;
}

pframe_t *pmm_alloc_page(void)
{
    return pmm_alloc_pages((pgcount_t)1);
}

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

#if DEBUG_MMU_mm_dealloc
    ppn_t ppn_dealloc;
    ppn_dealloc = convert_pframe2ppn(base_frame);
#endif

    BFallocator.bffa_insert_and_merge(base_frame, nsize);

#if DEBUG_MMU_mm_dealloc
    printf("pmm_free_pages::Frame has been deallocated!ppn:%ld,pa:%08lx,nsize:%u\n", ppn_dealloc, convert_ppn2pa(ppn_dealloc), nsize);
#endif

    spinlock_release(&pmm_lock, PmmLock_key);
}

static pframe_t *delete_and_reinsert(pgcount_t nsize)
{
    pframe_t *ret = NULL, *current_frame;
    struct list_head *current_entry; /* "Current*" is used for temp. */
    list_for_each(current_entry, &(pmm_free_list.list_linker))
    {
        current_frame = list_entry(current_entry, pframe_t, free_list_linker);
#if DEBUG_MMU_deleteAndReinsert
        printf("delete_and_reinsert::current_frame ppn:%ld,nsize:%u\n", convert_pframe2ppn(current_frame), current_frame->nsize);
#endif
        if (current_frame->nsize >= nsize)
        {
            ret = current_frame;
            break;
        }
    }
    /* If found the free block we need. */
    if (ret != NULL)
    {
        /* Delete this entry in pmm_free_list&pmm_free_addr_list. */
        list_del(&(ret->free_list_linker));
        list_del(&(ret->free_addr_list_linker));
        /* There are remaining memories in this block. */
        if (ret->nsize > nsize)
        {
            pframe_t *reinsert_frame = ret + nsize;
            reinsert_frame->nsize = ret->nsize - nsize;
            if ((&(pmm_free_list.list_linker))->next == &(pmm_free_list.list_linker))
            {
                list_add(&(reinsert_frame->free_list_linker), &(pmm_free_list.list_linker));
                list_add(&(reinsert_frame->free_addr_list_linker), &(pmm_free_addr_list.list_linker));
                goto f1;
            }
            /* Reinsert the remaining block into the free lists. */
            list_for_each(current_entry, &(pmm_free_list.list_linker))
            {
                if ((list_entry(current_entry, pframe_t, free_list_linker))->nsize >= reinsert_frame->nsize)
                {
                    /* Modify the remaining frame's entry in pmm_free_list. */
                    list_add_tail(&(reinsert_frame->free_list_linker), current_entry);
                    /* Modify this frame's entry in pmm_free_addr_list. */
                    list_add(&(reinsert_frame->free_addr_list_linker), (ret->free_addr_list_linker).prev);
                    break;
                }
                /* "reinsert_frame" is the largest block in pmm_free_list,add it into the pmm_free_list at last. */
                else if ((list_entry(current_entry, pframe_t, free_list_linker))->nsize < reinsert_frame->nsize && current_entry->next == &(pmm_free_list.list_linker))
                {
                    /* Modify the remaining frame's entry in pmm_free_list. */
                    list_add(&(reinsert_frame->free_list_linker), current_entry);
                    /* Modify this frame's entry in pmm_free_addr_list. */
                    list_add(&(reinsert_frame->free_addr_list_linker), (ret->free_addr_list_linker).prev);
                    break;
                }
            }
        }
    f1:
        pmm_free_list.fnsize = pmm_free_list.fnsize - nsize;
        pmm_free_addr_list.fnsize = pmm_free_addr_list.fnsize - nsize;
    }
#if DEBUG_MMU_deleteAndReinsert
    printf("delete_and_reinsert::pmm_free_list.fnsize:%u,pmm_free_addr_list.fnsize:%u\n", pmm_free_list.fnsize, pmm_free_addr_list.fnsize);
#endif
    return ret;
}

/* @param base_frame 被回收的物理块的首个物理页pframe_t地址
 * @param nsize 被回收的物理块的大小（含有几个物理页）
 */
static void insert_and_merge(pframe_t *base_frame, pgcount_t nsize)
{
    pframe_t *current_frame;
    struct list_head *current_entry;

    /* Insert into the pmm_free_addr_list frist,then merge if needed,finally insert into pmm_free_list. */
    pmm_free_addr_list.fnsize = pmm_free_addr_list.fnsize + nsize;
    if (list_empty(&(pmm_free_addr_list.list_linker)))
    {
        list_add(&(base_frame->free_addr_list_linker), &(pmm_free_addr_list.list_linker));
    }
    else
    {
        list_for_each(current_entry, &(pmm_free_addr_list.list_linker))
        {
            current_frame = list_entry(current_entry, pframe_t, free_addr_list_linker);
            if (current_frame > base_frame)
            {
                list_add_tail(&(base_frame->free_addr_list_linker), current_entry);
                break;
            }
            /* "base_frame" is the highest addr in memory,add it into the pmm_free_addr_list at last. */
            else if (current_entry->next == &(pmm_free_addr_list.list_linker))
            {
                list_add(&(base_frame->free_addr_list_linker), current_entry);
                break;
            }
        }
    }

    /* After inserted into pmm_free_addr_list,check if needs merge. */
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

#if DEBUG_MMU_insertAndMerge
    printf("insert_and_merge::base_frame:%ld prev_frame_in_addr_list:%ld next_frame_in_addr_list:%ld\n", convert_pframe2ppn(base_frame),
           convert_pframe2ppn(prev_frame_in_addr_list), convert_pframe2ppn(next_frame_in_addr_list));
#endif

    /* 先与后面的合并，因为可能存在需要同时合并前面和后面的情况。 */
    pframe_t *frame_closest_after = base_frame + base_frame->nsize;
    if (next_frame_in_addr_list != NULL)
    {
        if (frame_closest_after == next_frame_in_addr_list) /* It means need merge with the after block. */
        {
            list_del(&(next_frame_in_addr_list->free_addr_list_linker));
            list_del(&(next_frame_in_addr_list->free_list_linker));
            /* 注意baseppn->nsize在此时发生了变化，变成了与后面合并后的总的nsize大小，要使用回收的大小使用参数nsize。 */
            base_frame->nsize = base_frame->nsize + next_frame_in_addr_list->nsize;
            merged_frame = base_frame;
        }
    }
    if (prev_frame_in_addr_list != NULL)
    {
        pframe_t *frame_closest_forward = prev_frame_in_addr_list + prev_frame_in_addr_list->nsize;
        if (frame_closest_forward == base_frame) /* It means need merge with the forwrd block. */
        {
            list_del(&(base_frame->free_addr_list_linker));
            list_del(&(prev_frame_in_addr_list->free_list_linker));
            prev_frame_in_addr_list->nsize = prev_frame_in_addr_list->nsize + base_frame->nsize;
            merged_frame = prev_frame_in_addr_list;
        }
    }

#if DEBUG_MMU_insertAndMerge
    printf("insert_and_merge::merged_frame ppn:%ld,merged_frame->nsize %u\n", convert_pframe2ppn(merged_frame), merged_frame->nsize);
#endif

    /* After merge,insert into pmm_free_list. */
    pmm_free_list.fnsize = pmm_free_list.fnsize + nsize;
    if (list_empty(&(pmm_free_list.list_linker)))
    {
        list_add(&(merged_frame->free_list_linker), &(pmm_free_list.list_linker));
    }
    else
    {
        list_for_each(current_entry, &(pmm_free_list.list_linker))
        {
            if ((list_entry(current_entry, pframe_t, free_list_linker))->nsize >= merged_frame->nsize)
            {
                list_add_tail(&(merged_frame->free_list_linker), current_entry);
                break;
            }
            /* "reinsert_frame" is the largest block in pmm_free_list,add it into the pmm_free_list at last. */
            else if ((list_entry(current_entry, pframe_t, free_list_linker))->nsize < merged_frame->nsize && current_entry->next == &(pmm_free_list.list_linker))
            {
                list_add(&(merged_frame->free_list_linker), current_entry);
                break;
            }
        }
    }
}
