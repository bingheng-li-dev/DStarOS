/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/**
 * @file dcache_test.c
 * @brief 目录项缓存（LRU dcache）内核态自检
 * @details DEBUG_SUITE 为 SUITE_DCACHE 时由 debug_suite_run() 调用，
 *   跑完直接关机。覆盖七组：基本命中、LRU 复活、父引用与祖先链、删除逐出、
 *   重命名后子树仍可用、水位线与内存归还、进程退出归还 cwd 引用。
 *
 *   SMP 并发那一组不在这里——两个 hart 同时 open/close 同一批路径由
 *   SUITE_FILE（user/filetest.c 的双子进程阶段）覆盖，
 *   那条路径走的是真实 syscall 壳，连 vfs_big_lock 一起压到。
 */

#include "debug.h"

#if DEBUG_SUITE == SUITE_DCACHE

#include "suites.h"
#include "ktest.h"
#include "vfs.h"
#include "pmm.h"
#include "slab.h"
#include "kmalloc.h"
#include "console.h"
#include "stringops.h"
#include "errorcode.h"
#include "proc.h"

#define DCACHE_TEST_DIR   "/dctest"
#define DCACHE_TEST_SUB   "/dctest/sub"
#define DCACHE_TEST_DEEP  "/dctest/sub/deep.txt"
#define DCACHE_TEST_CWD   "/dccwd"
#define DCACHE_TEST_FLOOD 150   /* 水位线用例造的条目数，需明显大于 DCACHE_MAX_UNUSED */

static ktest_t dcache_kt = { "dcachetest", 0, 0 };

static void expect(bool cond, const char *what)
{
    ktest_expect(&dcache_kt, cond, what);
}

static void stats_of(dcache_stats_t *s)
{
    vfs_dcache_get_stats(s);
}

/* 清空整条 LRU：nr 给足够大即可（vfs_dcache_shrink 内部按差值算目标）*/
static void drain_all(void)
{
    vfs_dcache_shrink(0xffffffffu);
}

/* 建一个内容为 text 的文件；返回是否成功 */
static bool make_file(const char *path, const char *text)
{
    file_t *f = vfs_open(path, O_CREAT | O_RDWR | O_TRUNC, NULL);
    if (f == NULL)
    {
        return false;
    }
    bool ok = true;
    if (text != NULL)
    {
        int len = (int)strlen(text);
        ok = (vfs_write(f, text, len) == len);
    }
    vfs_close(f);
    return ok;
}

/* 组 1：基本命中——同一路径重复解析不再落到底层 lookup */
static void test_hit(void)
{
    dcache_stats_t a, b, c;

    drain_all();

    dentry_t *d1 = vfs_lookup(DCACHE_TEST_DEEP);
    expect(d1 != NULL, "lookup deep.txt succeeds");
    if (d1 == NULL)
    {
        return;
    }
    vfs_dentry_put(d1);
    stats_of(&a);

    /* 第二趟：三个分量应当全部命中缓存，一次 i_op->lookup 都不该发生 */
    dentry_t *d2 = vfs_lookup(DCACHE_TEST_DEEP);
    stats_of(&b);
    expect(d2 == d1, "second lookup returns the very same dentry object");
    expect(b.misses == a.misses, "second lookup produces no cache miss");
    expect(b.hits > a.hits, "second lookup counted as hits");
    expect(b.revives > a.revives, "leaf was revived from LRU");
    vfs_dentry_put(d2);

    /* 不同路径不能互相命中 */
    dentry_t *nx = vfs_lookup("/dctest/sub/nosuch.txt");
    stats_of(&c);
    expect(nx == NULL, "unrelated path does not hit the cache");
    expect(c.misses > b.misses, "unrelated path counted as a miss");
}

/* 组 2：复活——入队与出队严格成对 */
static void test_revive(void)
{
    drain_all();

    dentry_t *d = vfs_lookup(DCACHE_TEST_DEEP);
    if (d == NULL)
    {
        expect(false, "revive: lookup deep.txt");
        return;
    }
    expect(list_empty(&d->d_lru), "in-use dentry is not on the LRU");

    vfs_dentry_put(d);
    expect(d->d_ref == 0, "released dentry has d_ref == 0");
    expect(!list_empty(&d->d_lru), "released dentry entered the LRU");

    dentry_t *again = vfs_lookup(DCACHE_TEST_DEEP);
    expect(again == d, "cache hit returns the cached object");
    expect(list_empty(&d->d_lru), "revived dentry left the LRU");
    expect(d->d_ref == 1, "revived dentry has exactly one holder");
    vfs_dentry_put(again);
}

/* 组 3：父引用——缓存一个叶子会把整条祖先链钉住，回收时自底向上正确释放 */
static void test_parent_ref(void)
{
    dcache_stats_t before, after;

    drain_all();
    stats_of(&before);
    expect(before.nr_unused == 0, "LRU is empty after a full drain");

    dentry_t *leaf = vfs_lookup(DCACHE_TEST_DEEP);
    if (leaf == NULL)
    {
        expect(false, "parent ref: lookup deep.txt");
        return;
    }
    vfs_dentry_put(leaf);

    /* 祖先链必须还活着：sub 与 dctest 各自只剩"一个子目录项"这一份引用 */
    dentry_t *sub  = leaf->d_parent;
    dentry_t *top  = sub->d_parent;
    expect(strncmp(sub->d_name, "sub", VFS_NAME_MAX) == 0, "leaf's parent is sub");
    expect(strncmp(top->d_name, "dctest", VFS_NAME_MAX) == 0, "sub's parent is dctest");
    expect(sub->d_ref == 1, "sub is pinned by its cached child only");
    expect(top->d_ref == 1, "dctest is pinned by its cached child only");
    expect(list_empty(&sub->d_lru), "a dentry with children is never on the LRU");
    expect(list_empty(&top->d_lru), "a dentry with children is never on the LRU");
    dcache_stats_t pinned;
    stats_of(&pinned);
    expect(pinned.nr_unused == 1, "only the leaf sits on the LRU");

    /* 全清：叶子走了 sub 变叶子、sub 走了 dctest 变叶子，一次 drain 应当把三层
     * 全部收掉——根目录被 vfs_root_dentry 钉着，不参与。 */
    drain_all();
    stats_of(&after);
    expect(after.nr_unused == 0, "drain empties the LRU");
    expect(after.evicts == before.evicts + 3, "drain freed exactly leaf+sub+dctest");
}

/* 组 4：删除逐出——unlink 掉一个正在 LRU 上的文件 */
static void test_unlink_evict(void)
{
    drain_all();

    dentry_t *d = vfs_lookup(DCACHE_TEST_DEEP);
    if (d == NULL)
    {
        expect(false, "unlink evict: lookup deep.txt");
        return;
    }
    vfs_dentry_put(d);
    expect(!list_empty(&d->d_lru), "target is cached before unlink");

    dcache_stats_t before, after;
    stats_of(&before);
    expect(vfs_unlink(DCACHE_TEST_DEEP) == ENO0_NO_ERROR, "unlink cached file");
    stats_of(&after);
    expect(after.evicts > before.evicts, "unlinked dentry was really freed");

    /* 逐出之后不能再被命中 */
    dentry_t *gone = vfs_lookup(DCACHE_TEST_DEEP);
    expect(gone == NULL, "unlinked path no longer resolves");

    /* 父目录空了，rmdir 必须成功——判空看的是 d_subdirs，缓存不能让它误报非空 */
    expect(vfs_rmdir(DCACHE_TEST_SUB) == ENO0_NO_ERROR, "rmdir of the emptied dir");

    /* 复原，后面几组还要用 */
    expect(vfs_mkdir(DCACHE_TEST_SUB, 0755) == ENO0_NO_ERROR, "recreate sub");
    expect(make_file(DCACHE_TEST_DEEP, "deep"), "recreate deep.txt");
}

/* 组 5：重命名——目录改名之后，缓存在子树里的后代必须仍然能用，而且是命中缓存打开的。
 * 路径若存进 inode，改名只更新被改的那一个，后代全部指向旧路径；这组用例守着
 * "路径沿 dentry 链现推"这个性质。 */
static void test_rename_subtree(void)
{
    dcache_stats_t before, after;

    drain_all();

    expect(vfs_mkdir("/dctest/rn", 0755) == ENO0_NO_ERROR, "mkdir /dctest/rn");
    expect(make_file("/dctest/rn/c.txt", "hello"), "create /dctest/rn/c.txt");

    /* 让 c.txt 进缓存 */
    dentry_t *c = vfs_lookup("/dctest/rn/c.txt");
    expect(c != NULL, "cache /dctest/rn/c.txt");
    if (c != NULL)
    {
        vfs_dentry_put(c);
    }

    expect(vfs_rename("/dctest/rn", "/dctest/rn2") == ENO0_NO_ERROR, "rename dir");

    stats_of(&before);
    file_t *f = vfs_open("/dctest/rn2/c.txt", O_RDONLY, NULL);
    stats_of(&after);
    expect(f != NULL, "open child through the new directory name");
    /* 后代没有被改名连累：仍在缓存里，这一次解析一个 miss 都不该有 */
    expect(after.misses == before.misses, "cached descendant survived the rename");
    if (f != NULL)
    {
        char buf[8];
        memset(buf, 0, sizeof(buf));
        ssize_t n = vfs_read(f, buf, 5);
        expect(n == 5 && strncmp(buf, "hello", 5) == 0, "child content still correct");
        vfs_close(f);
    }

    /* 旧名字必须彻底消失 */
    dentry_t *old = vfs_lookup("/dctest/rn/c.txt");
    expect(old == NULL, "old directory name no longer resolves");

    /* 第二种情形：改名的时候后代正被打开（d_ref > 0）。这种后代剪枝够不着，
     * 只有"路径不再存进 inode"才治得了。ftruncate 走 i_op->truncate，会重新推导
     * 一次路径——若还是旧路径，f_open 根本找不到这个文件。 */
    file_t *held = vfs_open("/dctest/rn2/c.txt", O_RDWR, NULL);
    expect(held != NULL, "open descendant before renaming its directory");
    expect(vfs_rename("/dctest/rn2", "/dctest/rn3") == ENO0_NO_ERROR,
           "rename a directory that has an open descendant");
    if (held != NULL)
    {
        expect(vfs_ftruncate(held, 2) == ENO0_NO_ERROR,
               "ftruncate an open descendant after its directory was renamed");
        vfs_close(held);
    }

    file_t *re = vfs_open("/dctest/rn3/c.txt", O_RDONLY, NULL);
    expect(re != NULL, "reopen through the second new directory name");
    if (re != NULL)
    {
        char buf[8];
        memset(buf, 0, sizeof(buf));
        ssize_t n = vfs_read(re, buf, sizeof(buf) - 1);
        expect(n == 2 && strncmp(buf, "he", 2) == 0,
               "the truncation landed on the right file");
        vfs_close(re);
    }

    expect(vfs_unlink("/dctest/rn3/c.txt") == ENO0_NO_ERROR, "cleanup c.txt");
    expect(vfs_rmdir("/dctest/rn3") == ENO0_NO_ERROR, "cleanup /dctest/rn3");
}

/* 组 6：水位线与内存归还——造出远超上限的条目，确认驻留数被压住、清空后内存还回去 */
static void test_watermark(void)
{
    char path[VFS_PATH_MAX];
    dcache_stats_t st;
    int created = 0;

    drain_all();
    slab_reclaim_all();
    uint64_t free_before   = pmm_free_list.fnsize;
    uint32_t dentry_before = dentry_cache->nr_inuse;
    uint32_t inode_before  = inode_cache->nr_inuse;

    for (int i = 0; i < DCACHE_TEST_FLOOD; i++)
    {
        memcpy(path, "/dctest/f000", 13);
        path[9]  = (char)('0' + (i / 100) % 10);
        path[10] = (char)('0' + (i / 10) % 10);
        path[11] = (char)('0' + i % 10);
        if (!make_file(path, NULL))
        {
            break;
        }
        created++;
    }
    expect(created == DCACHE_TEST_FLOOD, "created the whole flood batch");

    /* 逐个解析并立刻释放：每一个都会进 LRU，总数远超上限 */
    for (int i = 0; i < created; i++)
    {
        memcpy(path, "/dctest/f000", 13);
        path[9]  = (char)('0' + (i / 100) % 10);
        path[10] = (char)('0' + (i / 10) % 10);
        path[11] = (char)('0' + i % 10);
        dentry_t *d = vfs_lookup(path);
        if (d != NULL)
        {
            vfs_dentry_put(d);
        }
    }

    stats_of(&st);
    expect(st.nr_unused <= DCACHE_MAX_UNUSED, "LRU stays under the high watermark");
    expect(st.nr_unused >= DCACHE_LOW_WATER, "batch shrink does not over-collect");
    expect(st.evicts > 0, "watermark actually triggered eviction");

    uint64_t free_peak = pmm_free_list.fnsize;
    expect(free_peak < free_before, "a full cache really does occupy memory");

    /* 回收之后再访问同一批路径仍然功能正确，只是变成 miss */
    memcpy(path, "/dctest/f000", 13);
    file_t *f = vfs_open(path, O_RDONLY, NULL);
    expect(f != NULL, "file still openable after its dentry was evicted");
    if (f != NULL)
    {
        vfs_close(f);
    }

    /* 清空缓存并删掉这批文件，内存必须回到起点 */
    for (int i = 0; i < created; i++)
    {
        memcpy(path, "/dctest/f000", 13);
        path[9]  = (char)('0' + (i / 100) % 10);
        path[10] = (char)('0' + (i / 10) % 10);
        path[11] = (char)('0' + i % 10);
        vfs_unlink(path);
    }
    drain_all();
    slab_reclaim_all();

    stats_of(&st);
    expect(st.nr_unused == 0, "LRU empty after the final drain");
    /* 精确判据看 slab 占用：缓存过的 dentry/inode 必须一个不剩地还回去 */
    expect(dentry_cache->nr_inuse == dentry_before, "every cached dentry was freed");
    expect(inode_cache->nr_inuse == inode_before, "every cached inode was freed");
    /* 空闲页数只与"缓存最满的那一刻"比：其它 hart 可能在测量窗口内借还整页，
     * 直接跟 free_before 对齐会偶发差几页。
     * "有没有泄漏"的精确判据是上面两条 nr_inuse，这里只确认页确实吐回去了。 */
    uint64_t free_after = pmm_free_list.fnsize;
    expect(free_after > free_peak, "cache pages went back to the PMM");
    printf("[dcachetest] free pages: before=%ld peak=%ld after=%ld\n",
           free_before, free_peak, free_after);
}

/* ============================================================
 * 组 7：进程退出归还 cwd 引用
 *
 * 这一组必须跑在大锁之外：worker 自己要 vfs_lock()，它退出时 do_exit 里
 * 归还 cwd 也要，握着锁 fork 会当场死锁。
 * ============================================================ */

static void *cwd_worker(void *arg)
{
    (void)arg;
    vfs_lock();
    vfs_chdir(DCACHE_TEST_CWD);
    vfs_unlock();
    return NULL; /* 落到 kernel_thread_entry 里的 do_exit */
}

/* 取 DCACHE_TEST_CWD 的引用计数：lookup 拿到的那一个 + 子目录项数 */
static int cwd_dir_ref(void)
{
    vfs_lock();
    dentry_t *d = vfs_lookup(DCACHE_TEST_CWD);
    int ref = (d != NULL) ? d->d_ref : -1;
    vfs_dentry_put(d);
    vfs_unlock();
    return ref;
}

static void test_exit_releases_cwd(void)
{
    vfs_lock();
    vfs_mkdir(DCACHE_TEST_CWD, 0755);
    vfs_unlock();

    int ref_before = cwd_dir_ref();
    expect(ref_before > 0, "cwd dir resolvable before the fork");

    int16_t pid = create_kernel_thread_by_fork(cwd_worker, NULL, 0);
    expect(pid > 0, "forked the chdir worker");
    if (pid > 0)
    {
        int status = 0;
        expect(do_wait(-1, &status, 0) == pid, "reaped the chdir worker");
    }

    int ref_after = cwd_dir_ref();
    expect(ref_after == ref_before, "exiting process gives its cwd reference back");
    if (ref_after != ref_before)
    {
        printf("[dcachetest] cwd d_ref: before=%d after=%d\n", ref_before, ref_after);
    }

    vfs_lock();
    vfs_rmdir(DCACHE_TEST_CWD);
    vfs_unlock();
}

static void setup(void)
{
    vfs_mkdir(DCACHE_TEST_DIR, 0755);
    vfs_mkdir(DCACHE_TEST_SUB, 0755);
    make_file(DCACHE_TEST_DEEP, "deep");
}

/**
 * @brief 目录项缓存自检入口
 */
void run_dcache_tests(void)
{
    ktest_reset(&dcache_kt);
    printf("=== dcachetest start ===\n");

    vfs_lock();   /* 整段测试独占 VFS 大锁：vfs.c 内部不再重复加锁 */
    setup();
    test_hit();
    test_revive();
    test_parent_ref();
    test_unlink_evict();
    test_rename_subtree();
    test_watermark();
    vfs_dcache_stats();
    vfs_unlock();

    test_exit_releases_cwd();

    ktest_done(&dcache_kt);
}

#endif /* DEBUG_SUITE == SUITE_DCACHE */
