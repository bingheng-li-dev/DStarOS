/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/**
 * @file slab_test.c
 * @brief slab 分配器内核态自检
 * @details 由 proc_init()（proc.c）在 DEBUG_SLAB_TEST 打开时调用 run_slab_tests()，
 *   跑完直接关机。覆盖四组：基本分配/释放/复用、通用尺寸类 kmalloc 往返、
 *   碎片化下的分档效果、空页回收与 slab_reclaim_all()。
 */

#include "debug.h"

#if DEBUG_SLAB_TEST

#include "slab.h"
#include "pmm.h"
#include "kmalloc.h"
#include "console.h"
#include "stringops.h"
#include "memtype.h"

#define SLAB_TEST_MAX_OBJS 400
#define SLAB_TEST_BIG_PAGES 16   /* 耗尽用例每块的页数 */

/* 耗尽用例的块数上限，必须大于"全部空闲内存能切出多少块"，否则循环先撞上限退出，
 * `n1 < 上限` 那条断言就失去了意义（它验的是 kmalloc 在真正耗尽时安静返回 NULL，
 * 而不是撞上 panic）。所以按 MEMORY_END 推导，不写死。 */
#define SLAB_TEST_MAX_BLOCKS \
    (((MEMORY_END - KERNEL_START) / PGSIZE) / SLAB_TEST_BIG_PAGES + 16)

static void *test_objs[SLAB_TEST_MAX_OBJS];
/* 耗尽用例专用，与 test_objs 分开：两者的容量判据完全不同（见 SLAB_TEST_MAX_BLOCKS）*/
static void *test_blocks[SLAB_TEST_MAX_BLOCKS];
static int slab_pass;
static int slab_fail;

static void expect(bool cond, const char *what)
{
    if (cond)
    {
        slab_pass++;
    }
    else
    {
        slab_fail++;
        printf("[slabtest] FAIL: %s\n", what);
    }
}

/* 固定种子的 LCG，保证碎片化用例可复现 */
static uint32_t lcg_state = 0x12345678u;

static uint32_t lcg_next(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (lcg_state >> 16) & 0x7fffu;
}

static uint32_t count_list(struct list_head *head)
{
    uint32_t n = 0;
    struct list_head *pos;
    list_for_each(pos, head)
    {
        n++;
    }
    return n;
}

static bool obj_in_slab_page(void *obj, uint32_t obj_size)
{
    virAddr_t va = (virAddr_t)obj;
    if ((va % 8) != 0)
    {
        return false;
    }
    return (va % PGSIZE) + obj_size <= PGSIZE;
}

/* 组 1：基本分配 / 地址互不重叠 / 全释放后复用同一批页 */
static void test_basic(void)
{
    kmem_cache_t *cache = slab_cache_create("slabtest-64", 64);
    expect(cache != NULL, "create 64B cache");
    if (cache == NULL)
    {
        return;
    }
    expect(cache->objs_per_slab == PGSIZE / 64, "objs_per_slab == 64");

    for (int i = 0; i < 200; i++)
    {
        test_objs[i] = slab_cache_alloc(cache);
    }

    bool all_ok = true;
    for (int i = 0; i < 200; i++)
    {
        if (test_objs[i] == NULL || !obj_in_slab_page(test_objs[i], 64))
        {
            all_ok = false;
            break;
        }
        /* 分配即清零是既有不变式，vfs_dentry_create 等调用点依赖它 */
        for (int b = 0; b < 64; b++)
        {
            if (((uint8_t *)test_objs[i])[b] != 0)
            {
                all_ok = false;
                break;
            }
        }
    }
    expect(all_ok, "200 objects valid, page-bounded and zeroed");

    bool no_overlap = true;
    for (int i = 0; i < 200 && no_overlap; i++)
    {
        for (int j = i + 1; j < 200; j++)
        {
            if (test_objs[i] == test_objs[j])
            {
                no_overlap = false;
                break;
            }
        }
    }
    expect(no_overlap, "200 objects pairwise distinct");
    expect(cache->nr_inuse == 200, "nr_inuse == 200");
    expect(cache->nr_slabs >= 200 / (PGSIZE / 64), "nr_slabs covers 200 objects");

    /* 写标记，释放后再分配应拿到同一批地址（页被复用而不是重新向 PMM 要） */
    uint32_t slabs_peak = cache->nr_slabs;
    for (int i = 0; i < 200; i++)
    {
        slab_cache_free(cache, convert_pa2pframe_flr(kva_to_pa((virAddr_t)test_objs[i])), test_objs[i]);
    }
    expect(cache->nr_inuse == 0, "nr_inuse back to 0");
    expect(cache->nr_slabs <= 1, "empty pages returned to PMM, one reserve kept");

    for (int i = 0; i < 200; i++)
    {
        test_objs[i] = slab_cache_alloc(cache);
    }
    expect(cache->nr_inuse == 200, "realloc 200 objects");
    expect(cache->nr_slabs <= slabs_peak, "realloc uses no more pages than before");
    for (int i = 0; i < 200; i++)
    {
        slab_cache_free(cache, convert_pa2pframe_flr(kva_to_pa((virAddr_t)test_objs[i])), test_objs[i]);
    }
}

/* 组 2：kmalloc/kfree 走通用尺寸类，覆盖所有档位与大于 2048 的整页路径 */
static void test_kmalloc(void)
{
    static const uint64_t sizes[] = {1, 8, 16, 17, 48, 64, 72, 96, 129, 200, 256,
                                     384, 512, 700, 1024, 2048, 2049, 4096, 9000};
    bool ok = true;
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
        void *p = kmalloc(sizes[i]);
        if (p == NULL)
        {
            ok = false;
            break;
        }
        for (uint64_t b = 0; b < sizes[i]; b++)
        {
            if (((uint8_t *)p)[b] != 0)
            {
                ok = false;
                break;
            }
        }
        memset(p, 0x5a, sizes[i]);
        kfree(p);
    }
    expect(ok, "kmalloc/kfree across all size classes, zero-filled");

    /* 交错分配释放：验证 kfree 能正确分派回各自的 cache */
    void *a = kmalloc(48);
    void *b = kmalloc(584);
    void *c = kmalloc(3 * PGSIZE);
    expect(a != NULL && b != NULL && c != NULL, "mixed-size kmalloc");
    kfree(b);
    kfree(a);
    kfree(c);
    expect(kmalloc(0) == NULL, "kmalloc(0) returns NULL");
}

/* 组 3：碎片化 —— 分配 400 个、随机释放 300 个、再分配 200 个，
 * 分档应让新分配集中到快满的页上，页数接近存活对象所需的理论下限 */
static void test_fragmentation(void)
{
    kmem_cache_t *cache = slab_cache_create("slabtest-80", 80);
    expect(cache != NULL, "create 80B cache");
    if (cache == NULL)
    {
        return;
    }
    uint32_t per_slab = cache->objs_per_slab;

    for (int i = 0; i < SLAB_TEST_MAX_OBJS; i++)
    {
        test_objs[i] = slab_cache_alloc(cache);
    }
    uint32_t slabs_full = cache->nr_slabs;
    printf("[slabtest] after 400 allocs: nr_slabs=%d nr_inuse=%d objs_per_slab=%d\n",
           slabs_full, cache->nr_inuse, per_slab);

    int freed = 0;
    while (freed < 300)
    {
        uint32_t idx = lcg_next() % SLAB_TEST_MAX_OBJS;
        if (test_objs[idx] != NULL)
        {
            slab_cache_free(cache, convert_pa2pframe_flr(kva_to_pa((virAddr_t)test_objs[idx])),
                            test_objs[idx]);
            test_objs[idx] = NULL;
            freed++;
        }
    }
    printf("[slabtest] after 300 random frees: nr_slabs=%d nr_inuse=%d\n",
           cache->nr_slabs, cache->nr_inuse);

    /* 再分配 200 个：分档生效时它们应填进已有的空洞，几乎不需要新页 */
    uint32_t slabs_before_refill = cache->nr_slabs;
    for (int i = 0; i < SLAB_TEST_MAX_OBJS && cache->nr_inuse < 300; i++)
    {
        if (test_objs[i] == NULL)
        {
            test_objs[i] = slab_cache_alloc(cache);
        }
    }
    uint32_t nr_full = count_list(&cache->full);
    printf("[slabtest] after refill to 300: nr_slabs=%d nr_inuse=%d nr_full_pages=%d\n",
           cache->nr_slabs, cache->nr_inuse, nr_full);
    expect(cache->nr_slabs <= slabs_before_refill,
           "refill reuses existing partial pages, no new page");

    /* 分档的可观测效果：新分配总是喂最满的页，300 个对象应挤进尽量少的页里，
     * 不分档（单条 partial 链表按 FIFO 取页）时新分配会摊到全部 8 页上，
     * 一页都填不满。理论下限是 ceil(300/51)=6 页全满，这里放宽到 4。 */
    expect(nr_full >= 4, "bucketing concentrates allocations into nearly-full pages");

    /* 全部释放：只应剩下保留页 */
    for (int i = 0; i < SLAB_TEST_MAX_OBJS; i++)
    {
        if (test_objs[i] != NULL)
        {
            slab_cache_free(cache, convert_pa2pframe_flr(kva_to_pa((virAddr_t)test_objs[i])),
                            test_objs[i]);
            test_objs[i] = NULL;
        }
    }
    expect(cache->nr_inuse == 0, "all objects freed");
    expect(cache->nr_slabs <= 1, "only the reserve page remains");
}

/* 反复要 SLAB_TEST_BIG_PAGES 页的大块直到 kmalloc 返回 NULL，返回拿到的块数 */
static int exhaust_big_blocks(void)
{
    int n = 0;
    while (n < SLAB_TEST_MAX_BLOCKS)
    {
        void *p = kmalloc(SLAB_TEST_BIG_PAGES * PGSIZE);
        if (p == NULL)
        {
            break;
        }
        test_blocks[n++] = p;
    }
    return n;
}

static void release_big_blocks(int n)
{
    for (int i = 0; i < n; i++)
    {
        kfree(test_blocks[i]);
        test_blocks[i] = NULL;
    }
}

/* 组 4：slab_reclaim_all 把保留页也吐回 PMM */
static void test_reclaim(void)
{
    pgcount_t before = pmm_free_list.fnsize;
    slab_reclaim_all();
    pgcount_t after = pmm_free_list.fnsize;
    /* 只打印不断言："回收前后空闲页数不减"的余量只有 1 页，另一个 hart 上的 vmm_test()
     * 随时可能借走一页。回收真正要保证的结果由下一条断言覆盖：大块分配拿得到。 */
    printf("[slabtest] reclaim: pmm_free_list.fnsize %d -> %d\n", before, after);

    void *big = kmalloc(64 * PGSIZE);
    expect(big != NULL, "large kmalloc after reclaim");
    if (big != NULL)
    {
        kfree(big);
    }

    /* 反复要大块直到连续块耗尽：应当安静地返回 NULL，而不是 panic */
    int n1 = exhaust_big_blocks();
    pgcount_t low1 = pmm_free_list.fnsize;
    expect(n1 > 0 && n1 < SLAB_TEST_MAX_BLOCKS, "kmalloc returns NULL on exhaustion instead of panic");
    release_big_blocks(n1);

    /* 判据是"再来一轮还能不能拿到同样多的块"，而不是把全局空闲页数跟快照对齐：
     * 另一个 hart 上的 vmm_test() 会临时占着若干页，全局计数里混着别人的账。
     * 块粒度 16 页远大于这点噪声，而且"还能再拿到 n 块"更强——它要求页真的回到 PMM
     * 并重新合并成了连续块。 */
    int n2 = exhaust_big_blocks();
    pgcount_t low2 = pmm_free_list.fnsize;
    release_big_blocks(n2);
    printf("[slabtest] exhaustion: round1 %d x %dKB (fnsize %d -> %d), round2 %d (-> %d)\n",
           n1, SLAB_TEST_BIG_PAGES * 4, after, low1, n2, low2);
    expect(n2 >= n1, "a full exhaust/release cycle gives every block back to the PMM");

    /* 归还之后必须重新合并成大块。上面两轮只要 16 页的块，merge-on-free 只做了
     * 一半（相邻块没并起来）照样能过；64 页这条才逼着 pmm_free_addr_list 真的合并回长连续段。 */
    void *big2 = kmalloc(64 * PGSIZE);
    expect(big2 != NULL, "64-page block still obtainable after the exhaust/release cycles");
    if (big2 != NULL)
    {
        kfree(big2);
    }

    /* 耗尽并全部归还之后，小对象分配仍要正常工作 */
    void *small = kmalloc(48);
    expect(small != NULL, "small kmalloc still works after exhaustion cycle");
    kfree(small);
}

void run_slab_tests(void)
{
    slab_pass = 0;
    slab_fail = 0;
    printf("=== slabtest start ===\n");
    test_basic();
    test_kmalloc();
    test_fragmentation();
    test_reclaim();
    slab_dump_stats();
    printf("=== slabtest done: %d pass  %d fail ===\n", slab_pass, slab_fail);
}

#endif /* DEBUG_SLAB_TEST */
