/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/*
 * bio.c - 块缓存
 *
 * 位于 diskio（FatFS 的磁盘层）与块设备驱动之间，按 (设备号, 块号) 缓存最近用过的 512 字节块。
 * 三个非显然的取舍：
 *
 * 1. 接口是拷贝式的（bio_read / bio_write 直接对应 disk_read / disk_write），不是 xv6 的
 *    bget / brelse。FatFS 总是把扇区拷进自己的 win[] 或调用者缓冲，从不持有缓存块指针，
 *    所以不需要引用计数与钉住。
 * 2. 多块读也走缓存，连续未命中的一段合并成一次设备读——大文件第二遍能命中缓存，
 *    未命中部分也不会退化成逐块读。
 * 3. 直写（write-through）：写先落到设备、成功后才更新缓存，缓存里永远没有脏块。
 *    这块板子没有可靠的关机钩子（sbi_shutdown 不工作），回写模式找不到安全的刷盘时机。
 *
 * 与 FatFS 自带缓存的分工：FATFS.win[] 是每个卷一个扇区的工作窗口（正在读改的那个 FAT / 目录扇区），
 * 命中时根本不会调到 disk_read；本缓存在它下面、跨卷共享，缓存最近读写过的大量扇区。
 *
 * 并发：设备 I/O 的串行由 VFS 大锁保证（FatFS 只在大锁内被调用，启动阶段单线程），
 * 本文件的自旋锁只保护链表、哈希与缓存块内容，调用驱动时不持锁。不能改用信号量：
 * fs_init() 在任何进程存在之前就要读盘，而信号量要取当前进程。
 */

#include "bio.h"
#include "list.h"
#include "sync.h"
#include "pmm.h"
#include "memtype.h"
#include "stringops.h"
#include "console.h"
#include "errorcode.h"

/* VF2 要装得下 4 MB 的 rootfs.img（8192 块），板上才看得出同一文件第二遍读的提速；
 * QEMU 故意取得比根文件系统里的数据量小，让回归套件真正走到淘汰路径。 */
#if defined(VF2)
#define BIO_NR_BUFS     16384U
#else
#define BIO_NR_BUFS     4096U
#endif
#define BIO_HASH_BITS   12
#define BIO_HASH_SIZE   (1U << BIO_HASH_BITS)
/* 批量放入缓存时每多少块释放一次锁：加载 ELF 这类大块读，一次拷几 MB 会让本核关中断太久 */
#define BIO_LOCK_BATCH  64U

typedef struct bio_buf
{
    struct list_head lru_linker;   /* 在 bio_lru 上：表头最近使用，表尾最久未用 */
    struct list_head hash_linker;  /* 有效时挂在哈希桶上；无效时自环 */
    uint64_t         lba;
    uint32_t         dev_id;
    bool             valid;
    uint8_t         *data;
} bio_buf_t;

static bio_buf_t *bio_bufs;
static struct list_head bio_lru;
static struct list_head bio_hash[BIO_HASH_SIZE];
static osslock_t bio_lock;
static bio_stats_t bio_stats;

/* 从 PMM 取连续、已清零的内存 */
static void *bio_alloc_zeroed(uint64_t bytes)
{
    pframe_t *frame = pmm_alloc_pages((pgcount_t)((bytes + PGSIZE - 1) / PGSIZE));
    return (frame != NULL) ? (void *)convert_pframe2kva(frame) : NULL;
}

/* (设备号, 块号) → 哈希桶：乘法散列取高位 */
static inline struct list_head *bio_bucket(uint32_t dev_id, uint64_t lba)
{
    uint64_t h = (lba ^ ((uint64_t)dev_id << 56)) * 0x9e3779b97f4a7c15ULL;
    return &bio_hash[h >> (64 - BIO_HASH_BITS)];
}

/* 查有效缓存块，未缓存返回 NULL；调用者持 bio_lock */
static bio_buf_t *bio_lookup_locked(uint32_t dev_id, uint64_t lba)
{
    struct list_head *head = bio_bucket(dev_id, lba);
    struct list_head *pos;
    list_for_each(pos, head)
    {
        bio_buf_t *b = list_entry(pos, bio_buf_t, hash_linker);
        if (b->dev_id == dev_id && b->lba == lba)
        {
            return b;
        }
    }
    return NULL;
}

/* 移到 LRU 表头（最近使用）；调用者持 bio_lock */
static void bio_touch_locked(bio_buf_t *b)
{
    list_del(&b->lru_linker);
    list_add(&b->lru_linker, &bio_lru);
}

/* 把一块内容放进缓存：已缓存则覆盖，否则占用 LRU 表尾那一块。
 * 调用者持 bio_lock；缓存里没有脏块，淘汰只是直接复用。 */
static void bio_store_locked(uint32_t dev_id, uint64_t lba, const uint8_t *src)
{
    bio_buf_t *b = bio_lookup_locked(dev_id, lba);
    if (b == NULL)
    {
        b = list_entry(bio_lru.prev, bio_buf_t, lru_linker);
        if (b->valid)
        {
            list_del_init(&b->hash_linker);
            bio_stats.evictions++;
        }
        b->dev_id = dev_id;
        b->lba = lba;
        b->valid = true;
        list_add(&b->hash_linker, bio_bucket(dev_id, lba));
    }
    memcpy(b->data, src, BDEV_BLOCK_SIZE);
    bio_touch_locked(b);
}

/* 把一段连续块放进缓存，每 BIO_LOCK_BATCH 块放一次锁 */
static void bio_store_range(uint32_t dev_id, uint64_t lba, const uint8_t *buf, uint32_t count)
{
    uint32_t k = 0;
    while (k < count)
    {
        uint32_t end = (count - k > BIO_LOCK_BATCH) ? k + BIO_LOCK_BATCH : count;
        irq_key_t key = spinlock_acquire(&bio_lock);
        for (; k < end; k++)
        {
            bio_store_locked(dev_id, lba + k, buf + (uint64_t)k * BDEV_BLOCK_SIZE);
        }
        spinlock_release(&bio_lock, key);
    }
}

/**
 * @brief 初始化块缓存：一次性分配全部缓存块
 * @note 必须早于任何文件系统挂载。分配失败直接 panic——启动阶段连这几 MB 都拿不到，
 *   后面也不可能正常运行。
 */
void bio_init(void)
{
    spinlock_init(&bio_lock);
    INIT_LIST_HEAD(&bio_lru);
    for (uint32_t i = 0; i < BIO_HASH_SIZE; i++)
    {
        INIT_LIST_HEAD(&bio_hash[i]);
    }

    bio_bufs = (bio_buf_t *)bio_alloc_zeroed((uint64_t)sizeof(bio_buf_t) * BIO_NR_BUFS);
    uint8_t *pool = (uint8_t *)bio_alloc_zeroed((uint64_t)BIO_NR_BUFS * BDEV_BLOCK_SIZE);
    if (bio_bufs == NULL || pool == NULL)
    {
        panic("bio_init: cannot allocate %u block buffers", BIO_NR_BUFS);
    }
    for (uint32_t i = 0; i < BIO_NR_BUFS; i++)
    {
        bio_buf_t *b = &bio_bufs[i];
        b->data = pool + (uint64_t)i * BDEV_BLOCK_SIZE;
        b->valid = false;
        INIT_LIST_HEAD(&b->hash_linker);
        list_add_tail(&b->lru_linker, &bio_lru);
    }
    printf("bio: %u block buffers (%u KB)\n", BIO_NR_BUFS, BIO_NR_BUFS / 2);
}

/**
 * @brief 读若干块，经缓存
 * @param[in]  dev   块设备（已就绪）
 * @param[in]  lba   起始块号
 * @param[out] buf   count * 512 字节
 * @param[in]  count 块数
 * @retval ENO0_NO_ERROR 全部读到
 * @return 其余为驱动 read_blocks 的错误码（遇到第一次设备读失败即返回）
 * @details 逐块查缓存：命中就拷贝；遇到未命中，把后面连续未命中的一段合并成一次设备读，
 *   读完整段放进缓存。越界由调用者（diskio）检查。
 */
int bio_read(bdev_t *dev, uint64_t lba, uint8_t *buf, uint32_t count)
{
    uint32_t i = 0;
    while (i < count)
    {
        irq_key_t key = spinlock_acquire(&bio_lock);
        bio_buf_t *b = bio_lookup_locked(dev->id, lba + i);
        if (b != NULL)
        {
            memcpy(buf + (uint64_t)i * BDEV_BLOCK_SIZE, b->data, BDEV_BLOCK_SIZE);
            bio_touch_locked(b);
            bio_stats.hits++;
            spinlock_release(&bio_lock, key);
            i++;
            continue;
        }
        uint32_t j = i + 1;
        while (j < count && bio_lookup_locked(dev->id, lba + j) == NULL)
        {
            j++;
        }
        spinlock_release(&bio_lock, key);

        int ret = dev->ops->read_blocks(dev, lba + i, buf + (uint64_t)i * BDEV_BLOCK_SIZE, j - i);
        if (ret != ENO0_NO_ERROR)
        {
            return ret;
        }
        bio_store_range(dev->id, lba + i, buf + (uint64_t)i * BDEV_BLOCK_SIZE, j - i);

        key = spinlock_acquire(&bio_lock);
        bio_stats.misses += j - i;
        bio_stats.dev_reads++;
        spinlock_release(&bio_lock, key);
        i = j;
    }
    return ENO0_NO_ERROR;
}

/**
 * @brief 写若干块（直写）：先写设备，成功后同步缓存
 * @param[in] dev   块设备（已就绪、可写）
 * @param[in] lba   起始块号
 * @param[in] buf   count * 512 字节
 * @param[in] count 块数
 * @retval ENO0_NO_ERROR 已写到设备
 * @return 其余为驱动 write_blocks 的错误码
 * @details 设备写失败时，这一段在设备上的内容不确定（可能写进去了一部分），
 *   对应的缓存块一律作废，之后的读会重新从设备取。
 */
int bio_write(bdev_t *dev, uint64_t lba, const uint8_t *buf, uint32_t count)
{
    int ret = dev->ops->write_blocks(dev, lba, buf, count);
    if (ret != ENO0_NO_ERROR)
    {
        irq_key_t key = spinlock_acquire(&bio_lock);
        for (uint32_t k = 0; k < count; k++)
        {
            bio_buf_t *b = bio_lookup_locked(dev->id, lba + k);
            if (b != NULL)
            {
                list_del_init(&b->hash_linker);
                b->valid = false;
                list_del(&b->lru_linker);
                list_add_tail(&b->lru_linker, &bio_lru);
            }
        }
        spinlock_release(&bio_lock, key);
        return ret;
    }

    bio_store_range(dev->id, lba, buf, count);
    irq_key_t key = spinlock_acquire(&bio_lock);
    bio_stats.dev_writes++;
    bio_stats.dev_write_blocks += count;
    spinlock_release(&bio_lock, key);
    return ENO0_NO_ERROR;
}

/**
 * @brief 取缓存统计的快照
 * @param[out] out 统计值
 */
void bio_get_stats(bio_stats_t *out)
{
    irq_key_t key = spinlock_acquire(&bio_lock);
    *out = bio_stats;
    spinlock_release(&bio_lock, key);
}
