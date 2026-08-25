#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "sync.h"
#include "pmm.h"
#include "vfs.h"
#include "errorcode.h"
#include "stringops.h"
#include "linux_abi.h"

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
    file_t *d = vfs_open("/testdir", O_RDONLY);
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

    /* ---- pending 暂存路径（本步骤最容易错的一条）----
     * 上面 24B 那条只验证了"不截断"，并没有触发 pending：'.'/'..' 来自合成阶段，
     * 放不下时只要不推进阶段机、下次重新生成即可。真正需要暂存的是**来自
     * f_readdir 的条目**——它的游标已经被 FatFS 消费掉了，放不下又不存起来就永久丢失。
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
    file_t *lf = vfs_open("/testdir/a_very_long_filename.txt", O_RDWR | O_CREAT);
    check("create long-named file", lf != NULL);
    if (lf)
    {
        vfs_close(lf);

        d = vfs_open("/testdir", O_RDONLY);
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
    file_t *bad = vfs_open("/testdir/sub.txt", O_RDONLY | O_DIRECTORY);
    check("open regular file with O_DIRECTORY -> fail", bad == NULL);
    if (bad)
    {
        vfs_close(bad);
    }

    kfree(buf);
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
    file_t *f = vfs_open("/hello.txt", O_RDWR | O_CREAT);
    check("open /hello.txt O_CREAT", f != NULL);
    if (f) {
        ssize_t w = vfs_write(f, "hello DStarOS", 13);
        check("write 13 bytes", w == 13);
        vfs_close(f);
    }

    char buf[32];
    memset(buf, 0, sizeof(buf));
    f = vfs_open("/hello.txt", O_RDONLY);
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
    f = vfs_open("/testdir/sub.txt", O_RDWR | O_CREAT);
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
    f = vfs_open("/testdir/sub.txt", O_RDWR);
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
    f = vfs_open("/testdir", O_RDONLY);
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

    vfs_unlock();
    printf("=== VFS test done: %d pass  %d fail ===\n\n", vfs_pass, vfs_fail);
}
