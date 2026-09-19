/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "pmm.h"
#include "kmalloc.h"
#include "memtype.h"
#include "slab.h"
#include "sync.h"
#include "vfs.h"

/* 只尝试一次，不做回收重试；重试由 kmalloc 在不持任何锁的层面上做。 */
static void *kmalloc_once(uint64_t size)
{
    if (size == 0)
    {
        return NULL;
    }

    kmem_cache_t *cache = slab_size_cache(size);
    if (cache != NULL)
    {
        return slab_cache_alloc(cache);
    }

    pframe_t *frame = pmm_alloc_pages(convert_pa2ppn_cil((phyAddr_t)size));
    if (frame == NULL)
    {
        return NULL;
    }
    return (void *)convert_pframe2kva(frame);
}

/* reclaim 必须留在这一层：pmm_alloc_pages() 内部持着 pmm_lock，就地调 slab_reclaim_all()
 * 既是对非重入锁的二次 acquire，锁序也与 cache->lock → pmm_lock 恰好相反。 */
void *kmalloc(uint64_t size)
{
    void *ptr = kmalloc_once(size);
    if (ptr != NULL)
    {
        return ptr;
    }
    /* 先吐目录项缓存再吐 slab 空闲页：dcache 里放的是真正可以丢弃的数据，
     * 丢了最多下次多解析一遍路径；slab 的空闲页则只是碎片的副产物。 */
    vfs_dcache_reclaim();
    slab_reclaim_all();
    return kmalloc_once(size);
}

void kfree(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

    pframe_t *frame = convert_pa2pframe_flr(kva_to_pa((virAddr_t)ptr));
    if (frame->slab_cache == NULL)
    {
        pmm_free_pages(frame);
    }
    else
    {
        slab_cache_free(frame->slab_cache, frame, ptr);
    }
}
