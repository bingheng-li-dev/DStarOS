/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/filetest.c —— 验证 POSIX 文件 syscall
 *
 * 这是 openat/lseek/readv/writev/fstat/newfstatat/getdents64/mkdirat/unlinkat/
 * renameat/chdir/getcwd/ftruncate/fcntl 这一整批 syscall 的**唯一真实验收手段**：
 * 它们全都要求真实的用户地址空间指针（syscall 边界的 strncpy_from_user 会拒绝内核
 * 指针），只能由真正的 U 态程序端到端触发，内核态自测覆盖不到。
 *
 * 自检式：每条断言打印 [PASS]/[FAIL]，末尾给出汇总行，供外层脚本 grep。
 * 不引入 libc，写法与 exectest.c 一致。
 */

#define __NR_getcwd      17
#define __NR_dup         23
#define __NR_dup3        24
#define __NR_fcntl       25
#define __NR_mkdirat     34
#define __NR_unlinkat    35
#define __NR_renameat2   276
#define __NR_ftruncate   46
#define __NR_chdir       49
#define __NR_openat      56
#define __NR_close       57
#define __NR_getdents64  61
#define __NR_lseek       62
#define __NR_read        63
#define __NR_write       64
#define __NR_readv       65
#define __NR_writev      66
#define __NR_newfstatat  79
#define __NR_fstat       80
#define __NR_exit        93
#define __NR_clone      220
#define __NR_wait4      260

#define AT_FDCWD      (-100)
#define AT_REMOVEDIR   0x200
#define AT_EMPTY_PATH  0x1000

#define O_RDONLY     0x0000
#define O_WRONLY     0x0001
#define O_RDWR       0x0002
#define O_CREAT      0x0040
#define O_EXCL       0x0080
#define O_TRUNC      0x0200
#define O_APPEND     0x0400
#define O_DIRECTORY  0x10000
#define O_CLOEXEC    0x80000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_DUPFD_CLOEXEC  1030
#define FD_CLOEXEC       1

#define DT_DIR 4
#define DT_REG 8

/* Linux errno（syscall 失败时返回其负值）*/
#define ENOENT  2
#define EBADF   9
#define EINVAL 22

#define S_IFMT  0xF000
#define S_IFREG 0x8000
#define S_IFDIR 0x4000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

/* Linux riscv64 asm-generic struct stat，128 字节，字段偏移必须与内核一致 */
struct linux_stat
{
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_rdev;
    unsigned long __pad1;
    long          st_size;
    int           st_blksize;
    int           __pad2;
    long          st_blocks;
    long          st_atime;
    long          st_atime_nsec;
    long          st_mtime;
    long          st_mtime_nsec;
    long          st_ctime;
    long          st_ctime_nsec;
    unsigned int  __unused_end[2];
};

struct linux_dirent64
{
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

struct iovec
{
    void         *iov_base;
    unsigned long iov_len;
};

static inline long syscall4(long nr, long a0, long a1, long a2, long a3)
{
    register long r_a7 asm("a7") = nr;
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a3 asm("a3") = a3;
    asm volatile("ecall"
                 : "+r"(r_a0)
                 : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a7)
                 : "memory");
    return r_a0;
}

/* renameat2 要五个参数（第五个是 flags），syscall4 不够用 */
static inline long syscall5(long nr, long a0, long a1, long a2, long a3, long a4)
{
    register long r_a7 asm("a7") = nr;
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a3 asm("a3") = a3;
    register long r_a4 asm("a4") = a4;
    asm volatile("ecall"
                 : "+r"(r_a0)
                 : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a7)
                 : "memory");
    return r_a0;
}
static long sys_write(int fd, const char *buf, unsigned long len)
{
    return syscall4(__NR_write, fd, (long)buf, (long)len, 0);
}
static long sys_read(int fd, char *buf, unsigned long len)
{
    return syscall4(__NR_read, fd, (long)buf, (long)len, 0);
}
static long sys_openat(int dirfd, const char *path, int flags, int mode)
{
    return syscall4(__NR_openat, dirfd, (long)path, flags, mode);
}
static long sys_close(int fd)            { return syscall4(__NR_close, fd, 0, 0, 0); }
static long sys_lseek(int fd, long off, int whence)
{
    return syscall4(__NR_lseek, fd, off, whence, 0);
}
static long sys_fstat(int fd, struct linux_stat *st)
{
    return syscall4(__NR_fstat, fd, (long)st, 0, 0);
}
static long sys_newfstatat(int dirfd, const char *path, struct linux_stat *st, int flags)
{
    return syscall4(__NR_newfstatat, dirfd, (long)path, (long)st, flags);
}
static long sys_getdents64(int fd, void *buf, unsigned long len)
{
    return syscall4(__NR_getdents64, fd, (long)buf, (long)len, 0);
}
static long sys_writev(int fd, const struct iovec *iov, int cnt)
{
    return syscall4(__NR_writev, fd, (long)iov, cnt, 0);
}
static long sys_readv(int fd, const struct iovec *iov, int cnt)
{
    return syscall4(__NR_readv, fd, (long)iov, cnt, 0);
}
static long sys_mkdirat(int dirfd, const char *path, int mode)
{
    return syscall4(__NR_mkdirat, dirfd, (long)path, mode, 0);
}
static long sys_unlinkat(int dirfd, const char *path, int flags)
{
    return syscall4(__NR_unlinkat, dirfd, (long)path, flags, 0);
}
static long sys_renameat(int odfd, const char *op, int ndfd, const char *np)
{
    /* riscv64 上没有 renameat(38)，只有 renameat2(276)——这个夹具此前发的是一个
     * 在本 ABI 上根本不存在的号，只因为内核也照着错的号接线才对得上。flags 恒 0。 */
    return syscall5(__NR_renameat2, odfd, (long)op, ndfd, (long)np, 0);
}
static long sys_chdir(const char *path)  { return syscall4(__NR_chdir, (long)path, 0, 0, 0); }
static long sys_getcwd(char *buf, unsigned long size)
{
    return syscall4(__NR_getcwd, (long)buf, (long)size, 0, 0);
}
static long sys_ftruncate(int fd, long len)
{
    return syscall4(__NR_ftruncate, fd, len, 0, 0);
}
static long sys_fcntl(int fd, int cmd, long arg)
{
    return syscall4(__NR_fcntl, fd, cmd, arg, 0);
}
static long sys_clone(void)              { return syscall4(__NR_clone, 0, 0, 0, 0); }
static long sys_wait4(long pid, int *ws) { return syscall4(__NR_wait4, pid, (long)ws, 0, 0); }
static void sys_exit(int code)
{
    syscall4(__NR_exit, code, 0, 0, 0);
    for (;;)
    {
    }
}

static unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
    {
        n++;
    }
    return n;
}

static int ustrcmp(const char *a, const char *b)
{
    unsigned long i = 0;
    while (a[i] && a[i] == b[i])
    {
        i++;
    }
    return (int)((unsigned char)a[i] - (unsigned char)b[i]);
}

static void puts_fd(int fd, const char *s)
{
    sys_write(fd, s, ustrlen(s));
}

static void put_long(long v)
{
    char tmp[24];
    int  i = 0;
    if (v < 0)
    {
        puts_fd(1, "-");
        v = -v;
    }
    if (v == 0)
    {
        puts_fd(1, "0");
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp))
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char out[25];
    int  j = 0;
    while (i > 0)
    {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';
    puts_fd(1, out);
}

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *name, int cond)
{
    puts_fd(1, cond ? "  [PASS] " : "  [FAIL] ");
    puts_fd(1, name);
    puts_fd(1, "\n");
    if (cond)
    {
        pass_count++;
    }
    else
    {
        fail_count++;
    }
}

/* 断言失败时把实际值也打出来，方便定位（期望值/实际值不符时光看名字看不出差在哪）*/
static void check_eq(const char *name, long got, long want)
{
    int ok = (got == want);
    check(name, ok);
    if (!ok)
    {
        puts_fd(1, "         want=");
        put_long(want);
        puts_fd(1, " got=");
        put_long(got);
        puts_fd(1, "\n");
    }
}

/* 在 getdents64 返回的记录序列里找 name，返回其 d_type；找不到返回 -1 */
static int find_dirent(const char *buf, long total, const char *name)
{
    long off = 0;
    while (off < total)
    {
        const struct linux_dirent64 *de = (const struct linux_dirent64 *)(buf + off);
        if (de->d_reclen == 0)
        {
            break;
        }
        if (ustrcmp(de->d_name, name) == 0)
        {
            return de->d_type;
        }
        off += de->d_reclen;
    }
    return -1;
}

/* 并发子进程做的事：在自己专属的文件上跑一轮 创建→写→回读→删除，
 * 两个子进程会被调度到两个 hart 上真并行，用来压 VFS 大锁。 */
static int concurrent_worker(const char *path)
{
    for (int round = 0; round < 20; round++)
    {
        long fd = sys_openat(AT_FDCWD, path, O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0)
        {
            return 1;
        }
        if (sys_write((int)fd, "concurrent", 10) != 10)
        {
            sys_close((int)fd);
            return 2;
        }
        if (sys_lseek((int)fd, 0, SEEK_SET) != 0)
        {
            sys_close((int)fd);
            return 3;
        }
        char rb[16];
        for (int i = 0; i < 16; i++)
        {
            rb[i] = 0;
        }
        if (sys_read((int)fd, rb, 10) != 10)
        {
            sys_close((int)fd);
            return 4;
        }
        rb[10] = '\0';
        if (ustrcmp(rb, "concurrent") != 0)
        {
            sys_close((int)fd);
            return 5;
        }
        sys_close((int)fd);
        if (sys_unlinkat(AT_FDCWD, path, 0) != 0)
        {
            return 6;
        }
    }
    return 0;
}

void _start(void)
{
    char buf[64];
    char dbuf[1024];
    struct linux_stat st;

    puts_fd(1, "\n=== filetest: POSIX file syscalls ===\n");

    /* ---------- openat / write / lseek / read ---------- */
    long fd = sys_openat(AT_FDCWD, "/filetest.txt", O_CREAT | O_RDWR, 0644);
    check("openat(O_CREAT|O_RDWR) ok", fd >= 0);
    if (fd < 0)
    {
        puts_fd(1, "filetest: cannot continue\n");
        sys_exit(1);
    }

    check_eq("write 13 bytes", sys_write((int)fd, "hello syscall", 13), 13);
    check_eq("lseek to 0", sys_lseek((int)fd, 0, SEEK_SET), 0);

    for (int i = 0; i < 64; i++)
    {
        buf[i] = 0;
    }
    check_eq("read back 13 bytes", sys_read((int)fd, buf, 13), 13);
    check("read content matches", ustrcmp(buf, "hello syscall") == 0);
    check_eq("lseek SEEK_END == 13", sys_lseek((int)fd, 0, SEEK_END), 13);

    /* ---------- fstat ---------- */
    check_eq("fstat ok", sys_fstat((int)fd, &st), 0);
    check_eq("fstat: st_size == 13", st.st_size, 13);
    check("fstat: S_ISREG", S_ISREG(st.st_mode));
    check("fstat: has exec bits (0111)", (st.st_mode & 0111) != 0);
    check_eq("fstat: st_blksize == 512", st.st_blksize, 512);

    /* ---------- writev / readv ---------- */
    check_eq("lseek to 0 before writev", sys_lseek((int)fd, 0, SEEK_SET), 0);
    struct iovec wv[2];
    wv[0].iov_base = (void *)"ABCDE";
    wv[0].iov_len  = 5;
    wv[1].iov_base = (void *)"12345";
    wv[1].iov_len  = 5;
    check_eq("writev 2 segments == 10", sys_writev((int)fd, wv, 2), 10);

    check_eq("lseek to 0 before readv", sys_lseek((int)fd, 0, SEEK_SET), 0);
    char rv0[5], rv1[5];
    struct iovec rv[2];
    rv[0].iov_base = rv0;
    rv[0].iov_len  = 5;
    rv[1].iov_base = rv1;
    rv[1].iov_len  = 5;
    check_eq("readv 2 segments == 10", sys_readv((int)fd, rv, 2), 10);
    check("readv seg0 == ABCDE", rv0[0] == 'A' && rv0[4] == 'E');
    check("readv seg1 == 12345", rv1[0] == '1' && rv1[4] == '5');

    /* ---------- ftruncate ---------- */
    check_eq("ftruncate to 5", sys_ftruncate((int)fd, 5), 0);
    check_eq("fstat after ftruncate", sys_fstat((int)fd, &st), 0);
    check_eq("ftruncate: st_size == 5", st.st_size, 5);

    /* ---------- fcntl ---------- */
    long dfd = sys_fcntl((int)fd, F_DUPFD, 5);
    check("fcntl(F_DUPFD, 5) >= 5", dfd >= 5);
    check_eq("fcntl(F_GETFD) == 0", sys_fcntl((int)dfd, F_GETFD, 0), 0);
    check_eq("fcntl(F_SETFD, FD_CLOEXEC)", sys_fcntl((int)dfd, F_SETFD, FD_CLOEXEC), 0);
    check_eq("fcntl(F_GETFD) == FD_CLOEXEC", sys_fcntl((int)dfd, F_GETFD, 0), FD_CLOEXEC);
    check("fcntl(F_GETFL) has O_RDWR", (sys_fcntl((int)dfd, F_GETFL, 0) & 3) == O_RDWR);
    long cfd = sys_fcntl((int)fd, F_DUPFD_CLOEXEC, 0);
    check("fcntl(F_DUPFD_CLOEXEC) ok", cfd >= 0);
    check_eq("F_DUPFD_CLOEXEC sets FD_CLOEXEC", sys_fcntl((int)cfd, F_GETFD, 0), FD_CLOEXEC);
    sys_close((int)cfd);
    sys_close((int)dfd);
    check_eq("close", sys_close((int)fd), 0);

    /* ---------- O_APPEND ----------
     * 这一组补于 2026-09-08：此前 72 条断言一条都没走过 O_APPEND，于是
     * "fatfs 读写回调从不按 file->f_pos 定位"这个洞一直没被发现——O_APPEND 是
     * **唯一**一条由 VFS 层绕过 f_op->lseek 直接改 f_pos 的路径，其余场景两份
     * 位置天然同步。详见 .claude/bugfixes.md。
     *
     * 判据刻意用**长度不同**的两段：等长覆盖写出来的结果和追加只差顺序，
     * 看 st_size 也看不出来。 */
    {
        const char *ap = "/append.txt";
        long afd = sys_openat(AT_FDCWD, ap, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        check("append: create", afd >= 0);
        check_eq("append: initial write", sys_write((int)afd, "AAAA", 4), 4);
        check_eq("append: close after initial write", sys_close((int)afd), 0);

        /* 重新以 O_APPEND 打开：f_pos 必须落在文件末尾，而不是 0 */
        afd = sys_openat(AT_FDCWD, ap, O_WRONLY | O_APPEND, 0);
        check("append: reopen with O_APPEND", afd >= 0);
        check_eq("append: write appends", sys_write((int)afd, "BB", 2), 2);
        check_eq("append: close", sys_close((int)afd), 0);

        afd = sys_openat(AT_FDCWD, ap, O_RDONLY, 0);
        check("append: reopen for read", afd >= 0);
        struct linux_stat ast;
        check_eq("append: fstat", sys_fstat((int)afd, &ast), 0);
        check_eq("append: st_size == 6 (not 4)", ast.st_size, 6);
        char abuf[8] = { 0 };
        check_eq("append: read back 6", sys_read((int)afd, abuf, sizeof(abuf)), 6);
        check("append: content is AAAABB", ustrcmp(abuf, "AAAABB") == 0);
        check_eq("append: close read fd", sys_close((int)afd), 0);
        check_eq("append: unlink", sys_unlinkat(AT_FDCWD, ap, 0), 0);
    }

    /* ---------- mkdirat + 子目录里建文件 ---------- */
    check_eq("mkdirat /fdir", sys_mkdirat(AT_FDCWD, "/fdir", 0755), 0);
    long ifd = sys_openat(AT_FDCWD, "/fdir/inner.txt", O_CREAT | O_RDWR, 0644);
    check("openat /fdir/inner.txt ok", ifd >= 0);
    if (ifd >= 0)
    {
        check_eq("write into subdir file", sys_write((int)ifd, "inner", 5), 5);
        sys_close((int)ifd);
    }

    /* ---------- getdents64 ---------- */
    long dirfd = sys_openat(AT_FDCWD, "/", O_RDONLY | O_DIRECTORY, 0);
    check("openat / with O_DIRECTORY ok", dirfd >= 0);
    if (dirfd >= 0)
    {
        for (int i = 0; i < 1024; i++)
        {
            dbuf[i] = 0;
        }
        long n = sys_getdents64((int)dirfd, dbuf, sizeof(dbuf));
        check("getdents64 / returns > 0", n > 0);
        puts_fd(1, "  / listing:");
        long off = 0;
        while (off < n)
        {
            const struct linux_dirent64 *de = (const struct linux_dirent64 *)(dbuf + off);
            if (de->d_reclen == 0)
            {
                break;
            }
            puts_fd(1, " ");
            puts_fd(1, de->d_name);
            off += de->d_reclen;
        }
        puts_fd(1, "\n");

        check_eq("getdents64: '.' is DT_DIR", find_dirent(dbuf, n, "."), DT_DIR);
        check_eq("getdents64: '..' is DT_DIR", find_dirent(dbuf, n, ".."), DT_DIR);
        check_eq("getdents64: 'fdir' is DT_DIR", find_dirent(dbuf, n, "fdir"), DT_DIR);
        check_eq("getdents64: 'filetest.txt' is DT_REG",
                 find_dirent(dbuf, n, "filetest.txt"), DT_REG);
        /* 'bin' 来自 rootfs 镜像（宿主机的 tools/build_rootfs.sh 建的），不是本用例造的。
         * 原先这里查的是 'hello'——内核启动时自己写进 ramdisk 的那个文件；阶段 9 之后
         * 程序改由镜像提供、那次 seed 已删除，换成查镜像里的目录，判据反而更强：
         * 它同时验了"外部造的条目能被列出来"和"目录的 d_type 是 DT_DIR"。 */
        check_eq("getdents64: 'bin' is DT_DIR", find_dirent(dbuf, n, "bin"), DT_DIR);
        sys_close((int)dirfd);
    }

    /* ---------- chdir + getcwd + 相对路径 ---------- */
    check_eq("chdir /fdir", sys_chdir("/fdir"), 0);
    for (int i = 0; i < 64; i++)
    {
        buf[i] = 0;
    }
    long cwdlen = sys_getcwd(buf, sizeof(buf));
    /* Linux 语义：返回写入的字节数，含结尾 '\0' —— "/fdir" 是 5 字符，故为 6 */
    check_eq("getcwd returns len incl NUL", cwdlen, 6);
    check("getcwd == /fdir", ustrcmp(buf, "/fdir") == 0);

    long relfd = sys_openat(AT_FDCWD, "inner.txt", O_RDONLY, 0);
    check("openat relative path after chdir", relfd >= 0);
    if (relfd >= 0)
    {
        for (int i = 0; i < 64; i++)
        {
            buf[i] = 0;
        }
        check_eq("read relative-opened file", sys_read((int)relfd, buf, 5), 5);
        check("relative file content == inner", ustrcmp(buf, "inner") == 0);
        sys_close((int)relfd);
    }
    check_eq("chdir back to /", sys_chdir("/"), 0);

    /* ---------- newfstatat ---------- */
    check_eq("newfstatat /fdir/inner.txt",
             sys_newfstatat(AT_FDCWD, "/fdir/inner.txt", &st, 0), 0);
    check_eq("newfstatat: size == 5", st.st_size, 5);
    check("newfstatat: S_ISREG", S_ISREG(st.st_mode));
    check_eq("newfstatat /fdir", sys_newfstatat(AT_FDCWD, "/fdir", &st, 0), 0);
    check("newfstatat: /fdir is S_ISDIR", S_ISDIR(st.st_mode));

    long efd = sys_openat(AT_FDCWD, "/fdir/inner.txt", O_RDONLY, 0);
    if (efd >= 0)
    {
        check_eq("newfstatat(AT_EMPTY_PATH) on fd",
                 sys_newfstatat((int)efd, "", &st, AT_EMPTY_PATH), 0);
        check_eq("AT_EMPTY_PATH: size == 5", st.st_size, 5);
        sys_close((int)efd);
    }

    /* ---------- renameat ---------- */
    check_eq("renameat inner.txt -> renamed.txt",
             sys_renameat(AT_FDCWD, "/fdir/inner.txt", AT_FDCWD, "/fdir/renamed.txt"), 0);
    check_eq("renameat: new name exists",
             sys_newfstatat(AT_FDCWD, "/fdir/renamed.txt", &st, 0), 0);
    check_eq("renameat: old name gone -> -ENOENT",
             sys_newfstatat(AT_FDCWD, "/fdir/inner.txt", &st, 0), -ENOENT);

    check_eq("mkdirat /fdir/sub", sys_mkdirat(AT_FDCWD, "/fdir/sub", 0755), 0);
    long ddfd = sys_openat(AT_FDCWD, "/fdir/sub/deep.txt", O_CREAT | O_RDWR, 0644);
    check_eq("openat /fdir/sub/deep.txt", ddfd >= 0 ? 1 : 0, 1);
    if (ddfd >= 0) { sys_close(ddfd); }
    check_eq("stat /fdir/sub/deep.txt", sys_newfstatat(AT_FDCWD, "/fdir/sub/deep.txt", &st, 0), 0);
    check_eq("unlinkat /fdir/sub/deep.txt", sys_unlinkat(AT_FDCWD, "/fdir/sub/deep.txt", 0), 0);
    check_eq("unlinkat /fdir/sub", sys_unlinkat(AT_FDCWD, "/fdir/sub", AT_REMOVEDIR), 0);

    /* ---------- unlinkat（文件 + AT_REMOVEDIR 删目录）---------- */
    check_eq("unlinkat /fdir/renamed.txt", sys_unlinkat(AT_FDCWD, "/fdir/renamed.txt", 0), 0);
    check_eq("unlinkat(AT_REMOVEDIR) /fdir",
             sys_unlinkat(AT_FDCWD, "/fdir", AT_REMOVEDIR), 0);
    check_eq("unlinkat /filetest.txt", sys_unlinkat(AT_FDCWD, "/filetest.txt", 0), 0);

    /* ---------- 负路径：错误码必须是具体值而不是笼统的 -1 ---------- */
    check_eq("openat deleted file -> -ENOENT",
             sys_openat(AT_FDCWD, "/filetest.txt", O_RDONLY, 0), -ENOENT);
    check_eq("openat nonexistent dir -> -ENOENT",
             sys_openat(AT_FDCWD, "/nosuchdir/x.txt", O_RDONLY, 0), -ENOENT);
    check_eq("openat relative with bad dirfd -> -EBADF",
             sys_openat(7, "relative.txt", O_RDONLY, 0), -EBADF);
    check_eq("lseek on closed fd -> -EBADF", sys_lseek(9, 0, SEEK_SET), -EBADF);
    check_eq("fstat on closed fd -> -EBADF", sys_fstat(9, &st), -EBADF);
    check_eq("fcntl bad cmd -> -EINVAL", sys_fcntl(1, 12345, 0), -EINVAL);
    check_eq("getdents64 on non-dir fd -> negative",
             sys_getdents64(1, dbuf, sizeof(dbuf)) < 0 ? 1 : 0, 1);

    /* ---------- 并发：两个子进程在两个 hart 上同时做文件操作 ---------- */
    puts_fd(1, "  -- concurrent file ops on 2 harts --\n");
    long p1 = sys_clone();
    if (p1 == 0)
    {
        sys_exit(concurrent_worker("/conc_a.txt"));
    }
    long p2 = sys_clone();
    if (p2 == 0)
    {
        sys_exit(concurrent_worker("/conc_b.txt"));
    }

    int st1 = 0;
    int st2 = 0;
    sys_wait4(-1, &st1);
    sys_wait4(-1, &st2);
    check_eq("concurrent worker A exit 0", (st1 >> 8) & 0xff, 0);
    check_eq("concurrent worker B exit 0", (st2 >> 8) & 0xff, 0);

    /* ---------- 汇总 ---------- */
    puts_fd(1, "=== filetest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");

    sys_exit(fail_count == 0 ? 0 : 1);
}
