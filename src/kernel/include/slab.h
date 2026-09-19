/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _SLAB_H_
#define _SLAB_H_

#include <stdint.h>
#include <stdbool.h>

#include "list.h"
#include "sync.h"
#include "pmm.h"

#define SLAB_NR_BUCKETS 4   /* partial 按 inuse 占比分档的档数 */
#define SLAB_MAX_CACHES 24  /* 静态 cache 表容量 */
#define SLAB_MIN_OBJ_SIZE 16
#define SLAB_MAX_OBJ_SIZE 2048

struct kmem_cache
{
    const char *name;
    uint32_t obj_size;      /* 8 字节对齐后的对象大小 */
    uint32_t objs_per_slab; /* PGSIZE / obj_size */
    uint32_t nr_slabs;      /* 统计：当前持有的 slab 页数（含 reserve） */
    uint32_t nr_inuse;      /* 统计：已分配出去的对象数 */
    struct list_head partial[SLAB_NR_BUCKETS];
    struct list_head full;
    pframe_t *reserve;      /* 保留的空闲页，NULL 表示没有 */
    osslock_t lock;
    bool valid;
};

void slab_init(void);
kmem_cache_t *slab_cache_create(const char *name, uint32_t size);
void *slab_cache_alloc(kmem_cache_t *cache);
void slab_cache_free(kmem_cache_t *cache, pframe_t *frame, void *obj);
void slab_reclaim_all(void);
void slab_dump_stats(void);

/* 通用尺寸类入口，size 超过 SLAB_MAX_OBJ_SIZE 时返回 NULL，由调用方回退到整页路径 */
kmem_cache_t *slab_size_cache(uint64_t size);

/* pmm_alloc_page() 失败时先 slab_reclaim_all() 再重试一次；调用者不得持有 pmm_lock */
pframe_t *slab_alloc_page_retry(void);

extern kmem_cache_t *vma_cache;
extern kmem_cache_t *mm_cache;
extern kmem_cache_t *inode_cache;
extern kmem_cache_t *file_cache;
extern kmem_cache_t *dentry_cache;
extern kmem_cache_t *pipe_cache;
extern kmem_cache_t *pcb_cache;
extern kmem_cache_t *fil_cache;

#endif
