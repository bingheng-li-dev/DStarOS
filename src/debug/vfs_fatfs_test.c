/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "sync.h"
#include "pmm.h"
#include "vfs.h"
#include "errorcode.h"
#include "stringops.h"
#include "linux_abi.h"
#include "slab.h"

static int vfs_pass = 0;
static int vfs_fail = 0;

static void check(const char *name, int cond)
{
    if (cond) {
        printf("  [PASS] %s\n", name);
        vfs_pass++;
    } else {
        printf("  [FAIL] %s\n", name);
        vfs_fail++;
    }
}

/* 在 buf 里的 dirent 记录序列中查找名为 name 的一条；找到返回其 d_type，否则返回 -1 */
static int find_dirent(const char *buf, int total, const char *name, int *out_count)
{
    int off  = 0;
    int type = -1;
    int n    = 0;

    while (off < total)
    {
        const struct linux_dirent64 *de = (const struct linux_dirent64 *)(buf + off);
        if (de->d_reclen == 0) /* 防御：reclen 为 0 会让循环原地死转 */
        {
            break;
        }
        n += 1;
        if (strncmp(de->d_name, name, VFS_NAME_MAX) == 0)
        {
            type = de->d_type;
        }
        off += de->d_reclen;
    }

    if (out_count)
    {
        *out_count = n;
    }
    return type;
}

/**
 * @brief 目录读取通路测试：vfs_open(目录) → vfs_getdents → linux_dirent64 记录解析
 * @note 调用时 /testdir 必须存在且含有 sub.txt（由 vfs_test 的前半段建好）。
 */
static void vfs_dir_test(void)
{
    char *buf = kmalloc(1024);
    if (!buf)
    {
        check("kmalloc dirent buffer", 0);
        return;
    }

    /* ---- 打开目录并读取全部条目 ---- */
    file_t *d = vfs_open("/testdir", O_RDONLY, NULL);
    check("open /testdir O_RDONLY", d != NULL);
    if (!d)
    {
        kfree(buf);
        return;
    }

    memset(buf, 0, 1024);
    int n = vfs_getdents(d, buf, 1024);
    check("getdents /testdir returns > 0", n > 0);

    int count = 0;
    int t_dot    = find_dirent(buf, n, ".", &count);
    int t_dotdot = find_dirent(buf, n, "..", NULL);
    int t_sub    = find_dirent(buf, n, "sub.txt", NULL);

    printf("  /testdir: %d entries in %d bytes\n", count, n);
    check("dirent: has synthesized '.'", t_dot == DT_DIR);
    check("dirent: has synthesized '..'", t_dotdot == DT_DIR);
    check("dirent: has sub.txt as DT_REG", t_sub == DT_REG);
    check("dirent: exactly 3 entries", count == 3);

    /* 所有记录的 d_reclen 必须 8 字节对齐，否则下一条记录的 d_off（8 字节字段）
     * 会落在非对齐地址上 */
    int off = 0, aligned = 1;
    while (off < n)
    {
        const struct linux_dirent64 *de = (const struct linux_dirent64 *)(buf + off);
        if (de->d_reclen == 0 || (de->d_reclen & 7) != 0)
        {
            aligned = 0;
            break;
        }
        off += de->d_reclen;
    }
    check("dirent: all d_reclen 8-byte aligned", aligned);
    check("dirent: records exactly fill returned length", off == n);

    /* ---- 再读一次应到 EOF ---- */
    int n2 = vfs_getdents(d, buf, 1024);
    check("getdents again -> EOF (0)", n2 == 0);

    /* ---- rewinddir：lseek(0, SEEK_SET) 后应能重新读到全部条目 ---- */
    off_t sret = vfs_lseek(d, 0, SEEK_SET);
    check("lseek(dir, 0, SEEK_SET) ok", sret == 0);
    memset(buf, 0, 1024);
    int n3 = vfs_getdents(d, buf, 1024);
    check("getdents after rewind == first read", n3 == n);

    /* ---- 目录上的非法操作 ---- */
    char tmp[8];
    ssize_t rr = vfs_read(d, tmp, sizeof(tmp));
    check("read() on dir -> IS_DIR", rr == ENO10_IS_DIR);
    sret = vfs_lseek(d, 16, SEEK_SET);
    check("lseek(dir, 16) -> INVAL", sret == (off_t)ENO6_INVAL_PARAM);

    /* ---- 小缓冲区：连一条都放不下应报 EINVAL；够一条时应只吐一条 ---- */
    vfs_lseek(d, 0, SEEK_SET);
    int nsmall = vfs_getdents(d, buf, 8);
    check("getdents with tiny buf -> INVAL", nsmall == ENO6_INVAL_PARAM);

    vfs_lseek(d, 0, SEEK_SET);
    int none = vfs_getdents(d, buf, 24); /* 刚好够 "." 一条（19+1 对齐到 24）*/
    check("getdents with 24B buf -> exactly one record", none == 24);
    check("that one record is '.'", find_dirent(buf, none, ".", NULL) == DT_DIR);

    /* ---- pending 暂存路径（最容易错的一条）----
     * 上面 24B 那条只验证了"不截断"，并没有触发 pending：'.'/'..' 来自合成阶段，
     * 放不下时只要不推进阶段机、下次重新生成即可。真正需要暂存的是来自
     * f_readdir 的条目——它的游标已经被 FatFS 消费掉了，放不下又不存起来就永久丢失。
     * 这里给 64 字节：刚好装下 '.'(24) + '..'(24)，第三条 sub.txt 需要 32 装不下，
     * 必须进 pending，并在下一次调用被原样吐出来。 */
    vfs_lseek(d, 0, SEEK_SET);
    memset(buf, 0, 1024);
    int p1 = vfs_getdents(d, buf, 64);
    check("getdents 64B -> only '.' and '..' (48B)", p1 == 48);
    check("64B read does not contain sub.txt yet", find_dirent(buf, p1, "sub.txt", NULL) == -1);
    memset(buf, 0, 1024);
    int p2 = vfs_getdents(d, buf, 1024);
    check("pending sub.txt is returned by next call", find_dirent(buf, p2, "sub.txt", NULL) == DT_REG);
    check("pending call returns exactly that one record", p2 == 32);

    vfs_close(d);

    /* ---- 长文件名（LFN）----
     * _USE_LFN 从 0 改成 3，唯一的动机就是"getdents64 一做出来长名就会暴露"：
     * 8.3 短名方案下这个名字会被截断成 A_VERY~1.TXT。这里验证 LFN 生效。 */
    const char *lname = "a_very_long_filename.txt";
    file_t *lf = vfs_open("/testdir/a_very_long_filename.txt", O_RDWR | O_CREAT, NULL);
    check("create long-named file", lf != NULL);
    if (lf)
    {
        vfs_close(lf);

        d = vfs_open("/testdir", O_RDONLY, NULL);
        if (d)
        {
            memset(buf, 0, 1024);
            int ln = vfs_getdents(d, buf, 1024);
            check("getdents sees long name verbatim (LFN works)",
                  find_dirent(buf, ln, lname, NULL) == DT_REG);
            vfs_close(d);
        }
        check("cleanup long-named file", vfs_unlink("/testdir/a_very_long_filename.txt") == ENO0_NO_ERROR);
    }

    /* ---- 打开普通文件带 O_DIRECTORY 应失败 ---- */
    file_t *bad = vfs_open("/testdir/sub.txt", O_RDONLY | O_DIRECTORY, NULL);
    check("open regular file with O_DIRECTORY -> fail", bad == NULL);
    if (bad)
    {
        vfs_close(bad);
    }

    kfree(buf);
}

/* ============================================================
 * 挂载 / 卸载：根 dentry 与根 inode 必须被回收
 *
 * 用一个自带的最小文件系统来测，而不是 fatfs 或 devfs：fatfs 只有一个 ramdisk 卷，
 * 重复挂载会跟已挂上的根打架；devfs 没有 i_op->lookup，只能靠创建时的引用把四个
 * 设备条目钉住不让逐出，根目录项的引用数因此恒 > 1，永远走的是"忙碌"分支。
 * nullfs 的根目录下什么都没有，卸载路径的两步（忙碌判定、归还根引用）才走得干净。
 * ============================================================ */

static int nullfs_inodes_destroyed;

static void nullfs_destroy_inode_cb(inode_t *inode)
{
    nullfs_inodes_destroyed += 1;
    kfree(inode);
}

static super_block_operations_t nullfs_sb_ops = {
    .alloc_inode   = NULL,
    .destory_inode = nullfs_destroy_inode_cb,
    .sync_fs       = NULL,
    .unmount       = NULL,
};

static dentry_t *nullfs_mount_cb(file_system_type_t *fst, const char *source, void *data)
{
    (void)fst;
    (void)source;
    (void)data;

    super_block_t *sb = alloc_super_block("nullfs", 0, 0, &nullfs_sb_ops, NULL);
    if (!sb)
    {
        return NULL;
    }
    inode_t *root_inode = (inode_t *)slab_cache_alloc(inode_cache);
    if (!root_inode)
    {
        destroy_super_block(sb);
        return NULL;
    }
    memset(root_inode, 0, sizeof(inode_t));
    root_inode->i_sb   = sb;
    root_inode->i_mode = S_IFDIR | 0755;

    dentry_t *root_dentry = dentry_create("", root_inode, NULL, NULL);
    if (!root_dentry)
    {
        nullfs_destroy_inode_cb(root_inode);
        destroy_super_block(sb);
        return NULL;
    }
    root_inode->i_dentry = root_dentry;
    sb->s_root_inode     = root_inode;
    return root_dentry;
}

static file_system_type_t nullfs_type = {
    .name    = "nullfs",
    .mount   = nullfs_mount_cb,
    .kill_sb = NULL,
    .next    = NULL,
};

/* 旧实现最多回溯 64 级，超出部分被静默丢掉；1 字符的目录名 66 级也才 140 来字节 */
#define GETCWD_DEEP_LEVELS 66

/* getcwd 的边界：挂载点下的 cwd、已删除的 cwd、深于 64 级的 cwd */
static void vfs_getcwd_edge_test(void)
{
    char cwd[VFS_PATH_MAX];

    check("chdir /dev", vfs_chdir("/dev") == ENO0_NO_ERROR);
    check("getcwd under a mountpoint -> ok", vfs_getcwd(cwd, sizeof(cwd)) == ENO0_NO_ERROR);
    check("getcwd under a mountpoint == \"/dev\"", strncmp(cwd, "/dev", 5) == 0);

    check("mkdir /gcwd_del", vfs_mkdir("/gcwd_del", 0755) == ENO0_NO_ERROR);
    check("chdir /gcwd_del", vfs_chdir("/gcwd_del") == ENO0_NO_ERROR);
    check("rmdir the cwd", vfs_rmdir("/gcwd_del") == ENO0_NO_ERROR);
    check("getcwd in a deleted dir -> NOSUCH", vfs_getcwd(cwd, sizeof(cwd)) == ENO5_NOSUCH_ENTRY);

    char expect_path[VFS_PATH_MAX];
    int elen = 0;
    memcpy(expect_path, "/gcwd_deep", 10);
    elen = 10;
    check("mkdir /gcwd_deep", vfs_mkdir("/gcwd_deep", 0755) == ENO0_NO_ERROR);
    check("chdir /gcwd_deep", vfs_chdir("/gcwd_deep") == ENO0_NO_ERROR);
    int made = 0;
    for (int i = 0; i < GETCWD_DEEP_LEVELS; i++)
    {
        if (vfs_mkdir("d", 0755) != ENO0_NO_ERROR || vfs_chdir("d") != ENO0_NO_ERROR)
        {
            break;
        }
        expect_path[elen++] = '/';
        expect_path[elen++] = 'd';
        made++;
    }
    expect_path[elen] = '\0';
    check("built a cwd deeper than 64 levels", made == GETCWD_DEEP_LEVELS);
    memset(cwd, 0xAA, sizeof(cwd));
    check("getcwd deeper than 64 levels -> ok", vfs_getcwd(cwd, sizeof(cwd)) == ENO0_NO_ERROR);
    check("getcwd deeper than 64 levels is the full path", strncmp(cwd, expect_path, VFS_PATH_MAX) == 0);

    for (int i = 0; i < made; i++)
    {
        vfs_chdir("..");
        vfs_rmdir("d");
    }
    check("chdir back to /", vfs_chdir("/") == ENO0_NO_ERROR);
    check("rmdir /gcwd_deep", vfs_rmdir("/gcwd_deep") == ENO0_NO_ERROR);
}

/* 挂载回调成功、之后在挂载点检查上失败：回调建好的根 inode / 根目录项必须被拆掉 */
static void vfs_mount_fail_test(void)
{
    vfs_dcache_shrink(0xffffffffu);
    uint32_t dentry_before = dentry_cache->nr_inuse;
    uint32_t inode_before  = inode_cache->nr_inuse;

    file_t *reg = vfs_open("/mnt_fail_file", O_WRONLY | O_CREAT, NULL);
    check("create a regular file to mount onto", reg != NULL);
    if (reg)
    {
        vfs_close(reg);
    }

    nullfs_inodes_destroyed = 0;
    check("mount onto a missing dir -> NOSUCH", vfs_mount("/no_such_dir", "nullfs", NULL) == ENO5_NOSUCH_ENTRY);
    check("mount onto a regular file -> NOT_DIR", vfs_mount("/mnt_fail_file", "nullfs", NULL) == ENO9_NOT_DIR);
    check("each failed mount tore down its root inode", nullfs_inodes_destroyed == 2);

    vfs_dcache_shrink(0xffffffffu);
    check("failed mounts leak no dentry", dentry_cache->nr_inuse == dentry_before);
    check("failed mounts leak no inode", inode_cache->nr_inuse == inode_before);
    check("unlink /mnt_fail_file", vfs_unlink("/mnt_fail_file") == ENO0_NO_ERROR);
}

static void vfs_unmount_test(void)
{
    check("register nullfs", register_filesystem(&nullfs_type) == ENO0_NO_ERROR);
    check("mkdir /mnt2", vfs_mkdir("/mnt2", 0755) == ENO0_NO_ERROR);

    /* 两次测量都先清空 LRU：nr_inuse 把缓存着的目录项也算在内，不清干净的话
     * 水位线在窗口里随手一次回收就会让两边对不上。 */
    vfs_dcache_shrink(0xffffffffu);
    uint32_t dentry_before = dentry_cache->nr_inuse;
    uint32_t inode_before  = inode_cache->nr_inuse;

    check("mount nullfs at /mnt2", vfs_mount("/mnt2", "nullfs", NULL) == ENO0_NO_ERROR);

    nullfs_inodes_destroyed = 0;
    check("unmount /mnt2", vfs_unmount("/mnt2") == ENO0_NO_ERROR);
    check("root inode went through destory_inode", nullfs_inodes_destroyed == 1);

    vfs_dcache_shrink(0xffffffffu);
    check("mount/unmount leaks no dentry", dentry_cache->nr_inuse == dentry_before);
    check("mount/unmount leaks no inode", inode_cache->nr_inuse == inode_before);

    /* 还有活引用的文件系统必须被拒绝，而不是把超级块从活着的 inode 脚下抽掉。
     * devfs 的四个设备条目各持根目录项一个引用，正好是这个场景。 */
    check("unmount a busy fs -> BUSY", vfs_unmount("/dev") == ENO4_BUSY);
    file_t *con = vfs_open("/dev/console", O_WRONLY, NULL);
    check("busy fs still usable after the refusal", con != NULL);
    if (con)
    {
        vfs_close(con);
    }

    vfs_mount_fail_test();

    check("rmdir /mnt2", vfs_rmdir("/mnt2") == ENO0_NO_ERROR);
    unregister_filesystem(&nullfs_type);
}

/**
 * @note 调用前必须确保 proc_init() 已经跑过——vfs_lock() 内部的 sem_down() 需要一个
 *   有效的当前 pcb（哪怕是 idle），在那之前调用会缺页异常（同 fs_init() 的教训）。
 *   当前未被任何地方调用；将来若要接线，应比照 debug.c 里 vmm_test() 的调用位置
 *   （main()，在 proc_init() 之后）。
 */
void vfs_test(void)
{
    printf("\n=== VFS basic test ===\n");
    vfs_lock();

    /* ---- write + read roundtrip ---- */
    file_t *f = vfs_open("/hello.txt", O_RDWR | O_CREAT, NULL);
    check("open /hello.txt O_CREAT", f != NULL);
    if (f) {
        ssize_t w = vfs_write(f, "hello DStarOS", 13);
        check("write 13 bytes", w == 13);
        vfs_close(f);
    }

    char buf[32];
    memset(buf, 0, sizeof(buf));
    f = vfs_open("/hello.txt", O_RDONLY, NULL);
    check("open /hello.txt O_RDONLY", f != NULL);
    if (f) {
        ssize_t r = vfs_read(f, buf, 13);
        check("read 13 bytes", r == 13);
        check("read content correct", strncmp(buf, "hello DStarOS", 13) == 0);
        vfs_close(f);
    }

    /* ---- stat file ---- */
    stat_t st;
    int ret = vfs_stat("/hello.txt", &st);
    check("stat /hello.txt", ret == ENO0_NO_ERROR);
    check("stat: is regular file", S_ISREG(st.st_mode));
    check("stat: size == 13", st.st_size == 13);

    /* ---- mkdir + stat dir ---- */
    ret = vfs_mkdir("/testdir", 0755);
    check("mkdir /testdir", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/testdir", &st);
    check("stat /testdir", ret == ENO0_NO_ERROR);
    check("stat: is directory", S_ISDIR(st.st_mode));

    /* ---- file in subdir ---- */
    f = vfs_open("/testdir/sub.txt", O_RDWR | O_CREAT, NULL);
    check("open /testdir/sub.txt O_CREAT", f != NULL);
    if (f) {
        ssize_t w = vfs_write(f, "sub", 3);
        check("write 3 bytes to subdir file", w == 3);
        vfs_close(f);
    }

    /* ---- truncate ---- */
    ret = vfs_truncate("/hello.txt", 5);
    check("truncate /hello.txt to 5", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/hello.txt", &st);
    check("stat after truncate: size == 5", st.st_size == 5);

    /* ---- rename ---- */
    ret = vfs_rename("/hello.txt", "/renamed.txt");
    check("rename /hello.txt -> /renamed.txt", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/renamed.txt", &st);
    check("stat /renamed.txt after rename", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/hello.txt", &st);
    check("stat old name -> NOSUCH", ret == ENO5_NOSUCH_ENTRY);

    /* ---- unlink ---- */
    ret = vfs_unlink("/renamed.txt");
    check("unlink /renamed.txt", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/renamed.txt", &st);
    check("stat deleted file -> NOSUCH", ret == ENO5_NOSUCH_ENTRY);

    /* ---- ftruncate（按已打开的 file 截断，不经路径）---- */
    f = vfs_open("/testdir/sub.txt", O_RDWR, NULL);
    check("reopen sub.txt for ftruncate", f != NULL);
    if (f)
    {
        ret = vfs_ftruncate(f, 1);
        check("vfs_ftruncate to 1", ret == ENO0_NO_ERROR);
        vfs_close(f);
        ret = vfs_stat("/testdir/sub.txt", &st);
        check("ftruncate: size == 1", ret == ENO0_NO_ERROR && st.st_size == 1);
    }
    /* 目录上 ftruncate 必须被拒 */
    f = vfs_open("/testdir", O_RDONLY, NULL);
    if (f)
    {
        check("ftruncate on dir -> IS_DIR", vfs_ftruncate(f, 0) == ENO10_IS_DIR);
        vfs_close(f);
    }

    /* ---- chdir + getcwd 往返 ----
     * 顺带验证 sys_getcwd 依赖的契约：vfs_getcwd 写进去的是一个以 '\0' 结尾的字符串，
     * 所以 syscall 壳里 "strlen(kpath)+1" 才是 Linux 要求的返回值（含结尾 '\0' 的长度）。 */
    char cwd[VFS_PATH_MAX];
    memset(cwd, 0xAA, sizeof(cwd));
    ret = vfs_getcwd(cwd, sizeof(cwd));
    check("getcwd at start -> ok", ret == ENO0_NO_ERROR);
    check("getcwd at start == \"/\"", strncmp(cwd, "/", 2) == 0);

    ret = vfs_chdir("/testdir");
    check("chdir /testdir", ret == ENO0_NO_ERROR);
    memset(cwd, 0xAA, sizeof(cwd));
    ret = vfs_getcwd(cwd, sizeof(cwd));
    check("getcwd after chdir -> ok", ret == ENO0_NO_ERROR);
    check("getcwd after chdir == \"/testdir\"", strncmp(cwd, "/testdir", 9) == 0);
    check("getcwd result is NUL-terminated", cwd[strlen(cwd)] == '\0');

    check("chdir onto a regular file -> NOT_DIR",
          vfs_chdir("/testdir/sub.txt") == ENO9_NOT_DIR);

    vfs_getcwd_edge_test();

    /* 必须切回根：后面的用例和 init 的其余流程都假定 cwd 是 "/" */
    ret = vfs_chdir("/");
    check("chdir back to /", ret == ENO0_NO_ERROR);

    /* ---- 目录读取（getdents64 后端）---- */
    vfs_dir_test();

    /* ---- rmdir (must be empty first) ---- */
    ret = vfs_unlink("/testdir/sub.txt");
    check("unlink /testdir/sub.txt", ret == ENO0_NO_ERROR);
    ret = vfs_rmdir("/testdir");
    check("rmdir /testdir", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/testdir", &st);
    check("stat deleted dir -> NOSUCH", ret == ENO5_NOSUCH_ENTRY);

    /* ---- 挂载 / 卸载 ---- */
    vfs_unmount_test();

    vfs_unlock();
    printf("=== VFS test done: %d pass  %d fail ===\n\n", vfs_pass, vfs_fail);
}
