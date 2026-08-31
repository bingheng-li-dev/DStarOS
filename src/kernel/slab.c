#include "slab.h"
#include "pmm.h"
#include "sync.h"
#include "stringops.h"
#include "console.h"
#include "memtype.h"
#include "vmm.h"
#include "vfs.h"
#include "proc.h"
#include "pipe.h"
#include "ff.h"

#define SLAB_NR_SIZE_CLASSES 12

static const uint32_t size_class_table[SLAB_NR_SIZE_CLASSES] = {
    16, 32, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 2048};

static const char *const size_class_name[SLAB_NR_SIZE_CLASSES] = {
    "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-96",
    "kmalloc-128", "kmalloc-192", "kmalloc-256", "kmalloc-384",
    "kmalloc-512", "kmalloc-768", "kmalloc-1024", "kmalloc-2048"};

static kmem_cache_t cache_table[SLAB_MAX_CACHES];
static osslock_t cache_table_lock;
static kmem_cache_t *kmalloc_caches[SLAB_NR_SIZE_CLASSES];

kmem_cache_t *vma_cache;
kmem_cache_t *mm_cache;
kmem_cache_t *inode_cache;
kmem_cache_t *file_cache;
kmem_cache_t *dentry_cache;
kmem_cache_t *pipe_cache;
kmem_cache_t *pcb_cache;
kmem_cache_t *fil_cache;

static int32_t bucket_of(kmem_cache_t *cache, pframe_t *frame);
static void slab_enqueue(kmem_cache_t *cache, pframe_t *frame);
static void slab_requeue(kmem_cache_t *cache, pframe_t *frame);
static pframe_t *slab_pick_partial(kmem_cache_t *cache);
static void slab_page_init(kmem_cache_t *cache, pframe_t *frame);

/**
 * @name slab_init
 * @brief 建立通用尺寸类 cache 与 8 个命名 cache
 * @note 必须在 MMU 开启之后、任何 kmalloc 调用之前由 hart 0 调用一次。
 *   slab 内部保存的全是 KVA，放在 MMU 前初始化会让 pmm_init_after_mmu_enable
 *   需要额外遍历修复每个 slab 页内的 freelist 指针。
 */
void slab_init(void)
{
    spinlock_init(&cache_table_lock);
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++)
    {
        cache_table[i].valid = false;
    }

    for (uint32_t i = 0; i < SLAB_NR_SIZE_CLASSES; i++)
    {
        kmalloc_caches[i] = slab_cache_create(size_class_name[i], size_class_table[i]);
    }

    /* 一律用 sizeof 而不是写死字节数：写小了会让对象越界踩到下一个对象 */
    vma_cache    = slab_cache_create("vma", sizeof(vma_t));
    mm_cache     = slab_cache_create("mm", sizeof(mm_t));
    inode_cache  = slab_cache_create("inode", sizeof(inode_t));
    file_cache   = slab_cache_create("file", sizeof(file_t));
    dentry_cache = slab_cache_create("dentry", sizeof(dentry_t));
    pipe_cache   = slab_cache_create("pipe", sizeof(pipe_t));
    pcb_cache    = slab_cache_create("pcb", sizeof(pcb_t));
    fil_cache    = slab_cache_create("fil", sizeof(FIL));

    printf("slab allocator inited!\n");
}

/**
 * @name slab_cache_create
 * @brief 在静态 cache 表里占用一个槽位并初始化
 * @param[in] name cache 名，仅用于统计输出，必须是常量字符串
 * @param[in] size 对象大小（字节），会向上对齐到 8 字节
 * @retval NULL 静态表已满或 size 越界
 * @return 新建的 cache
 */
kmem_cache_t *slab_cache_create(const char *name, uint32_t size)
{
    if (size == 0 || size > SLAB_MAX_OBJ_SIZE)
    {
        return NULL;
    }
    if (size < SLAB_MIN_OBJ_SIZE)
    {
        size = SLAB_MIN_OBJ_SIZE;
    }
    size = (size + 7u) & ~7u;

    kmem_cache_t *cache = NULL;
    irq_key_t cache_table_lock_key = spinlock_acquire(&cache_table_lock);
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++)
    {
        if (!cache_table[i].valid)
        {
            cache = &cache_table[i];
            break;
        }
    }
    if (cache == NULL)
    {
        spinlock_release(&cache_table_lock, cache_table_lock_key);
        panic("slab: cache table exhausted\n");
    }

    cache->name = name;
    cache->obj_size = size;
    cache->objs_per_slab = (uint32_t)PGSIZE / size;
    cache->nr_slabs = 0;
    cache->nr_inuse = 0;
    cache->reserve = NULL;
    for (uint32_t i = 0; i < SLAB_NR_BUCKETS; i++)
    {
        INIT_LIST_HEAD(&cache->partial[i]);
    }
    INIT_LIST_HEAD(&cache->full);
    spinlock_init(&cache->lock);
    cache->valid = true;
    spinlock_release(&cache_table_lock, cache_table_lock_key);
    return cache;
}

kmem_cache_t *slab_size_cache(uint64_t size)
{
    if (size == 0 || size > SLAB_MAX_OBJ_SIZE)
    {
        return NULL;
    }
    for (uint32_t i = 0; i < SLAB_NR_SIZE_CLASSES; i++)
    {
        if (size <= size_class_table[i])
        {
            return kmalloc_caches[i];
        }
    }
    return NULL;
}

/**
 * @name slab_cache_alloc
 * @brief 从 cache 取一个对象，取不到时向 PMM 要一页新的 slab
 * @param[in,out] cache 目标 cache
 * @retval NULL 物理内存耗尽
 * @return 已清零的对象地址（KVA）
 * @details 锁序固定为 cache->lock → PmmLock，因此向 PMM 要页之前必须先放开
 *   cache->lock。放锁期间另一个 hart 可能已经补上了页，所以拿回锁后一律回到
 *   循环开头重判，新页只作为保留页安置，绝不直接使用。
 */
void *slab_cache_alloc(kmem_cache_t *cache)
{
    pframe_t *frame;

    if (cache == NULL)
    {
        return NULL;
    }

    irq_key_t cache_lock_key = spinlock_acquire(&cache->lock);
    for (;;)
    {
        frame = slab_pick_partial(cache);
        if (frame != NULL)
        {
            break;
        }
        if (cache->reserve != NULL)
        {
            frame = cache->reserve;
            cache->reserve = NULL;
            list_add(&frame->slab_linker, &cache->partial[0]);
            break;
        }

        spinlock_release(&cache->lock, cache_lock_key);
        pframe_t *fresh = alloc_page();
        irq_key_t cache_lock_key = spinlock_acquire(&cache->lock);
        if (fresh == NULL)
        {
            spinlock_release(&cache->lock, cache_lock_key);
            return NULL;
        }
        if (cache->reserve == NULL)
        {
            slab_page_init(cache, fresh);
            cache->reserve = fresh;
            cache->nr_slabs++;
        }
        else
        {
            spinlock_release(&cache->lock, cache_lock_key);
            dealloc(fresh);
            /* 重新取锁：赋值给循环外的 key，不能再声明一个同名局部把它遮蔽掉 */
            cache_lock_key = spinlock_acquire(&cache->lock);
        }
    }

    void *obj = frame->slab_freelist;
    frame->slab_freelist = *(void **)obj;
    frame->slab_inuse++;
    cache->nr_inuse++;
    slab_requeue(cache, frame);
    spinlock_release(&cache->lock, cache_lock_key);

    memset(obj, 0, cache->obj_size);
    return obj;
}

/**
 * @name slab_cache_free
 * @brief 把对象还回它所属的 slab 页，整页空闲时回收或留作保留页
 * @param[in,out] cache 对象所属 cache
 * @param[in,out] frame 对象所在的 slab 页
 * @param[in] obj 对象地址（KVA）
 */
void slab_cache_free(kmem_cache_t *cache, pframe_t *frame, void *obj)
{
    irq_key_t cache_lock_key = spinlock_acquire(&cache->lock);

    *(void **)obj = frame->slab_freelist;
    frame->slab_freelist = obj;
    frame->slab_inuse--;
    cache->nr_inuse--;

    if (frame->slab_inuse == 0)
    {
        list_del(&frame->slab_linker);
        if (cache->reserve == NULL)
        {
            cache->reserve = frame;
        }
        else
        {
            frame->slab_cache = NULL;
            frame->slab_freelist = NULL;
            cache->nr_slabs--;
            spinlock_release(&cache->lock, cache_lock_key);
            dealloc(frame);
            return;
        }
    }
    else
    {
        slab_requeue(cache, frame);
    }

    spinlock_release(&cache->lock, cache_lock_key);
}

/**
 * @name slab_reclaim_all
 * @brief 把所有 cache 的保留页与整页空闲的 slab 页吐还给 PMM
 * @note 只能在不持有 PmmLock 的上下文里调用；逐 cache 加解锁，不用一把大锁罩全表。
 */
void slab_reclaim_all(void)
{
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++)
    {
        kmem_cache_t *cache = &cache_table[i];
        if (!cache->valid)
        {
            continue;
        }
        for (;;)
        {
            pframe_t *victim = NULL;
            irq_key_t cache_lock_key = spinlock_acquire(&cache->lock);
            if (cache->reserve != NULL)
            {
                victim = cache->reserve;
                cache->reserve = NULL;
            }
            else
            {
                struct list_head *pos, *n;
                list_for_each_safe(pos, n, &cache->partial[0])
                {
                    pframe_t *f = list_entry(pos, pframe_t, slab_linker);
                    if (f->slab_inuse == 0)
                    {
                        list_del(&f->slab_linker);
                        victim = f;
                        break;
                    }
                }
            }
            if (victim != NULL)
            {
                victim->slab_cache = NULL;
                victim->slab_freelist = NULL;
                cache->nr_slabs--;
            }
            spinlock_release(&cache->lock, cache_lock_key);
            if (victim == NULL)
            {
                break;
            }
            dealloc(victim);
        }
    }
}

pframe_t *slab_alloc_page_retry(void)
{
    pframe_t *frame = alloc_page();
    if (frame != NULL)
    {
        return frame;
    }
    slab_reclaim_all();
    return alloc_page();
}

void slab_dump_stats(void)
{
    printf("slab stats: name/objsize/objs_per_slab/nr_slabs/nr_inuse\n");
    for (uint32_t i = 0; i < SLAB_MAX_CACHES; i++)
    {
        kmem_cache_t *cache = &cache_table[i];
        if (!cache->valid)
        {
            continue;
        }
        irq_key_t cache_lock_key = spinlock_acquire(&cache->lock);
        uint32_t nr_slabs = cache->nr_slabs;
        uint32_t nr_inuse = cache->nr_inuse;
        uint32_t objs = cache->objs_per_slab;
        uint32_t osz = cache->obj_size;
        spinlock_release(&cache->lock, cache_lock_key);
        if (nr_slabs == 0 && nr_inuse == 0)
        {
            continue;
        }
        printf("  %s %d %d %d %d\n", cache->name, osz, objs, nr_slabs, nr_inuse);
    }
}

static int32_t bucket_of(kmem_cache_t *cache, pframe_t *frame)
{
    int32_t bucket = (int32_t)((uint32_t)frame->slab_inuse * SLAB_NR_BUCKETS / cache->objs_per_slab);
    if (bucket >= SLAB_NR_BUCKETS)
    {
        bucket = SLAB_NR_BUCKETS - 1;
    }
    return bucket;
}

static void slab_enqueue(kmem_cache_t *cache, pframe_t *frame)
{
    if (frame->slab_freelist == NULL)
    {
        list_add(&frame->slab_linker, &cache->full);
    }
    else
    {
        list_add(&frame->slab_linker, &cache->partial[bucket_of(cache, frame)]);
    }
}

static void slab_requeue(kmem_cache_t *cache, pframe_t *frame)
{
    list_del(&frame->slab_linker);
    slab_enqueue(cache, frame);
}

/* 从最高非空档取页：优先喂"快满的页"，让空闲对象集中到少数页上，
 * 整页空闲才有机会出现，Step 5 的空页回收才有东西可回收。 */
static pframe_t *slab_pick_partial(kmem_cache_t *cache)
{
    for (int32_t i = SLAB_NR_BUCKETS - 1; i >= 0; i--)
    {
        if (!list_empty(&cache->partial[i]))
        {
            return list_entry(cache->partial[i].next, pframe_t, slab_linker);
        }
    }
    return NULL;
}

static void slab_page_init(kmem_cache_t *cache, pframe_t *frame)
{
    virAddr_t base = convert_pframe2kva(frame);
    void *next = NULL;
    for (int32_t i = (int32_t)cache->objs_per_slab - 1; i >= 0; i--)
    {
        void *obj = (void *)(base + (uint64_t)i * cache->obj_size);
        *(void **)obj = next;
        next = obj;
    }
    frame->slab_freelist = next;
    frame->slab_inuse = 0;
    frame->slab_cache = cache;
    INIT_LIST_HEAD(&frame->slab_linker);
}
