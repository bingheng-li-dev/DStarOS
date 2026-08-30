#include "sbi.h"
#include "console.h"
#include "syscall.h"
#include "uaccess.h"
#include "cpu.h"
#include "proc.h"
#include "vfs.h"
#include "errorcode.h"
#include "linux_abi.h"
#include "kmalloc.h"
#include "stringops.h"
#include "pipe.h"
#include "tty.h"
#include "vmm.h"

/* read/write 的中转缓冲大小：一页，kmalloc 分配——内核栈只有 1 页 4KB
 * （KERNEL_STACKPSIZE 1），FatFS 调用链本来就深，不能在栈上开这么大的缓冲。 */
#define SYS_RW_BUF_SIZE PGSIZE
/* readv/writev 一次调用最多接受的 iovec 段数，防止不可信的 iovcnt 触发过大的 kmalloc */
#define SYS_IOV_MAX 64

/* 把 ubuf 的 len 字节写入 f，按 SYS_RW_BUF_SIZE 分块拷贝+落盘。
 * 调用方必须已经 vfs_lock()，kbuf 是调用方提供的 SYS_RW_BUF_SIZE 大小的中转缓冲
 * （writev 要对多个 iovec 段复用同一块，不在这里反复 kmalloc/kfree）。
 * @return 成功写出的字节数；仅当一个字节都没写出去时才返回负 ENO*（部分写出后出错，
 *   按 POSIX 语义返回已写字节数，不吞掉部分成功）。 */
static long do_write_locked(file_t *f, const char *ubuf, uint64_t len, char *kbuf)
{
    uint64_t done = 0;
    while (done < len)
    {
        uint64_t n = len - done;
        if (n > SYS_RW_BUF_SIZE)
        {
            n = SYS_RW_BUF_SIZE;
        }
        if (copy_from_user(kbuf, ubuf + done, n) != 0)
        {
            return done ? (long)done : ENO8_NULL_POINTER;
        }
        ssize_t w = f->f_op->write(f, kbuf, n);
        if (w < 0)
        {
            return done ? (long)done : (long)w; /* 透传底层错误码，而非折叠成固定值 */
        }
        done += (uint64_t)w;
        if ((uint64_t)w < n) /* 短写：底层没接收完，停 */
        {
            break;
        }
    }
    return (long)done;
}

/* 从 f 读最多 len 字节到 ubuf，单次 f_op->read 调用（不循环补满——
 * POSIX read() 允许短读，循环补满不是语义要求）。约束同 do_write_locked。 */
static long do_read_locked(file_t *f, char *ubuf, uint64_t len, char *kbuf)
{
    uint64_t n = len;
    if (n > SYS_RW_BUF_SIZE)
    {
        n = SYS_RW_BUF_SIZE;
    }
    ssize_t r = f->f_op->read(f, kbuf, n);
    if (r < 0)
    {
        return (long)r; /* 透传底层错误码，而非折叠成固定值 */
    }
    if (r > 0 && copy_to_user(ubuf, kbuf, (uint64_t)r) != 0)
    {
        return ENO8_NULL_POINTER;
    }
    return (long)r;
}

static long sys_write(int fd, const char *ubuf, uint64_t len)
{
    file_t *f = proc_fd_get(fd);
    if (!f || !f->f_op || !f->f_op->write)
    {
        return ENO19_BAD_FD;
    }

    char *kbuf = kmalloc(SYS_RW_BUF_SIZE);
    if (!kbuf)
    {
        return ENO1_NOMORE_MEM;
    }

    /* 管道/设备类 file 会自己阻塞（自带锁或 waitq），不能持 vfs_big_lock 睡——
     * 否则此后任何进程碰任何文件 syscall 都会卡死在同一把锁上 */
    bool need_lock = vfs_file_needs_lock(f);
    if (need_lock)
    {
        vfs_lock();
    }
    long ret = do_write_locked(f, ubuf, len, kbuf);
    if (need_lock)
    {
        vfs_unlock();
    }

    kfree(kbuf);
    return ret;
}

static long sys_read(int fd, char *ubuf, uint64_t len)
{
    file_t *f = proc_fd_get(fd);
    if (!f || !f->f_op || !f->f_op->read)
    {
        return ENO19_BAD_FD;
    }

    char *kbuf = kmalloc(SYS_RW_BUF_SIZE);
    if (!kbuf)
    {
        return ENO1_NOMORE_MEM;
    }

    bool need_lock = vfs_file_needs_lock(f);
    if (need_lock)
    {
        vfs_lock();
    }
    long ret = do_read_locked(f, ubuf, len, kbuf);
    if (need_lock)
    {
        vfs_unlock();
    }

    kfree(kbuf);
    return ret;
}

/* writev(fd, iov, iovcnt)：逐段调用 do_write_locked，遇到出错/短写就停，
 * 返回已写出的总字节数（一个字节都没写出去时返回负 ENO*）。 */
static long sys_writev(int fd, const struct iovec *uiov, int iovcnt)
{
    file_t *f = proc_fd_get(fd);
    if (!f || !f->f_op || !f->f_op->write)
    {
        return ENO19_BAD_FD;
    }
    if (iovcnt < 0 || iovcnt > SYS_IOV_MAX)
    {
        return ENO6_INVAL_PARAM;
    }
    if (iovcnt == 0)
    {
        return 0;
    }

    struct iovec *kiov = kmalloc(sizeof(struct iovec) * (size_t)iovcnt);
    if (!kiov)
    {
        return ENO1_NOMORE_MEM;
    }
    if (copy_from_user(kiov, uiov, sizeof(struct iovec) * (size_t)iovcnt) != 0)
    {
        kfree(kiov);
        return ENO8_NULL_POINTER;
    }

    char *kbuf = kmalloc(SYS_RW_BUF_SIZE);
    if (!kbuf)
    {
        kfree(kiov);
        return ENO1_NOMORE_MEM;
    }

    long total = 0;
    bool need_lock = vfs_file_needs_lock(f);
    if (need_lock)
    {
        vfs_lock();
    }
    for (int i = 0; i < iovcnt; i++)
    {
        if (kiov[i].iov_len == 0)
        {
            continue;
        }
        long r = do_write_locked(f, (const char *)kiov[i].iov_base, kiov[i].iov_len, kbuf);
        if (r < 0)
        {
            if (total == 0)
            {
                total = r; /* 还没写出任何字节，透传这一段的错误码 */
            }
            break;
        }
        total += r;
        if ((uint64_t)r < kiov[i].iov_len) /* 这一段短写，后面的段不再继续 */
        {
            break;
        }
    }
    if (need_lock)
    {
        vfs_unlock();
    }

    kfree(kbuf);
    kfree(kiov);
    return total;
}

/* readv(fd, iov, iovcnt)：逐段调用 do_read_locked，按段顺序填充，
 * 遇到短读（含 EOF）就停——不跨段"凑够"，这是 POSIX readv 的常见实现方式。 */
static long sys_readv(int fd, const struct iovec *uiov, int iovcnt)
{
    file_t *f = proc_fd_get(fd);
    if (!f || !f->f_op || !f->f_op->read)
    {
        return ENO19_BAD_FD;
    }
    if (iovcnt < 0 || iovcnt > SYS_IOV_MAX)
    {
        return ENO6_INVAL_PARAM;
    }
    if (iovcnt == 0)
    {
        return 0;
    }

    struct iovec *kiov = kmalloc(sizeof(struct iovec) * (size_t)iovcnt);
    if (!kiov)
    {
        return ENO1_NOMORE_MEM;
    }
    if (copy_from_user(kiov, uiov, sizeof(struct iovec) * (size_t)iovcnt) != 0)
    {
        kfree(kiov);
        return ENO8_NULL_POINTER;
    }

    char *kbuf = kmalloc(SYS_RW_BUF_SIZE);
    if (!kbuf)
    {
        kfree(kiov);
        return ENO1_NOMORE_MEM;
    }

    long total = 0;
    bool need_lock = vfs_file_needs_lock(f);
    if (need_lock)
    {
        vfs_lock();
    }
    for (int i = 0; i < iovcnt; i++)
    {
        if (kiov[i].iov_len == 0)
        {
            continue;
        }
        long r = do_read_locked(f, (char *)kiov[i].iov_base, kiov[i].iov_len, kbuf);
        if (r < 0)
        {
            if (total == 0)
            {
                total = r;
            }
            break;
        }
        total += r;
        if ((uint64_t)r < kiov[i].iov_len) /* 短读（含 EOF）：不再继续后面的段 */
        {
            break;
        }
    }
    if (need_lock)
    {
        vfs_unlock();
    }

    kfree(kbuf);
    kfree(kiov);
    return total;
}

/**
 * @brief openat(dirfd, path, flags, mode)
 * @note `mode`（权限位）当前被忽略——FAT 没有权限概念，vfs_open 内部固定按 0644/0755
 *   建 inode（见 fatfs_vfs.c 建 inode 的四处回调），与 O_CREAT 的调用方无关。
 * @note `dirfd` 只支持 `AT_FDCWD` 或路径本身是绝对路径这两种情况（此时 dirfd 被忽略）；
 *   传入其它 dirfd 值一律返回 `-EBADF`——真正的"相对某个已打开目录 fd 解析路径"需要
 *   `vfs_lookup` 支持从任意 dentry 起点解析，当前 VFS 没有这个能力，留给以后实测撞上再补。
 * @note `vfs_open` 目前失败时统一返回 NULL，无法区分"文件不存在"/"是目录却按文件打开"/
 *   "权限不足"等具体原因，这里统一按最常见的 ENOENT 处理——已知不精确，是 vfs_open
 *   自身尚未做错误码细分导致的限制，不是本函数引入的新问题。
 */
static long sys_openat(int dirfd, const char *upath, int flags, int mode)
{
    (void)mode;

    char kpath[VFS_PATH_MAX];
    long path_len = strncpy_from_user(kpath, upath, sizeof(kpath));
    if (path_len < 0)
    {
        return path_len;
    }

    if (dirfd != AT_FDCWD && kpath[0] != '/')
    {
        return ENO19_BAD_FD;
    }

    vfs_lock();
    file_t *f = vfs_open(kpath, flags);
    vfs_unlock();
    if (!f)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    int fd = proc_fd_alloc();
    if (fd < 0)
    {
        vfs_lock();
        vfs_close(f);
        vfs_unlock();
        return ENO18_TOO_MANY_FILES; /* fd 表满时 file 已经打开了，必须回滚关掉 */
    }

    proc_fd_install(fd, f);
    if (flags & O_CLOEXEC)
    {
        proc_fd_set_flags(fd, FD_CLOEXEC);
    }
    return fd;
}

/* lseek(fd, offset, whence)：vfs_lseek 已经处理了 SEEK_SET/CUR/END 与越界校验，
 * 这里只是薄壳。目录 fd 的特殊语义（仅允许 SEEK_SET 到 0 = rewinddir）留给
 * 目录读取通路落地之后——当前 vfs_open 还不支持打开目录，这个分支永远走不到，
 * 现在加上只是死代码。 */
static long sys_lseek(int fd, long offset, int whence)
{
    file_t *f = proc_fd_get(fd);
    if (!f)
    {
        return ENO19_BAD_FD;
    }
    /* 管道/设备没有偏移概念，且 vfs_lseek 的 SEEK_END 会解引用 f_inode——
     * 管道/设备的 f_inode 为空，真进去会空指针崩溃，必须在壳里挡住 */
    if (f->f_kind != FILE_KIND_VFS)
    {
        return ENO21_ILLEGAL_SEEK;
    }

    vfs_lock();
    off_t ret = vfs_lseek(f, (off_t)offset, whence);
    vfs_unlock();
    return (long)ret;
}

/**
 * @brief 内核内部 stat_t（4 字段）→ Linux riscv64 ABI 的 struct linux_stat（128 字节）
 * @details 布局填充：
 *   `st_mode`/`st_size`/`st_nlink` 直接搬；`st_blksize` 固定 512（FatFS 扇区大小）；
 *   `st_blocks` 按 512 字节块数向上取整；`st_dev`/`st_rdev`/uid/gid/三个时间戳
 *   FAT 没有对应概念或目前没有时钟源，统一填 0（时间戳待 `clock_gettime` 实现后再回填）。
 *   **`st_mode` 不能是 0**——BusyBox `ls` 靠 `S_ISDIR(st_mode)` 判类型，
 *   `ash` 执行程序前靠 `st_mode & 0111` 判可执行位，这也是 fatfs_vfs.c 建 inode 时
 *   必须真的填权限位的原因（见该文件的改动）。
 */
static void stat_to_linux(const stat_t *ks, struct linux_stat *ls)
{
    memset(ls, 0, sizeof(*ls));
    ls->st_ino     = ks->st_ino;
    ls->st_mode    = ks->st_mode;
    ls->st_nlink   = ks->st_nlink;
    ls->st_size    = (int64_t)ks->st_size;
    ls->st_blksize = 512;
    ls->st_blocks  = ((int64_t)ks->st_size + 511) / 512;
}

/**
 * @brief 给管道/console 这类没有 f_inode 的 file 合成一份 stat_t
 * @details 只在调用方已确认 `f->f_kind != FILE_KIND_VFS` 时调用——真实 VFS 文件
 *   走 vfs_fstat（有真 inode）。管道填 S_IFIFO|0600、console 填 S_IFCHR|0620，
 *   st_size 恒为 0（管道无固定大小概念，console 同理）。
 */
static int fill_stat_nonvfs(file_t *f, stat_t *kst)
{
    memset(kst, 0, sizeof(*kst));
    kst->st_nlink = 1;
    if (f->f_kind == FILE_KIND_PIPE)
    {
        kst->st_mode = S_IFIFO | 0600;
    }
    else /* FILE_KIND_DEVICE */
    {
        kst->st_mode = S_IFCHR | 0620;
    }
    return ENO0_NO_ERROR;
}

static long sys_fstat(int fd, struct linux_stat *ustatbuf)
{
    file_t *f = proc_fd_get(fd);
    if (!f)
    {
        return ENO19_BAD_FD;
    }

    stat_t kst;
    int ret;
    if (f->f_kind == FILE_KIND_VFS)
    {
        vfs_lock();
        ret = vfs_fstat(f, &kst);
        vfs_unlock();
    }
    else
    {
        ret = fill_stat_nonvfs(f, &kst);
    }
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }

    struct linux_stat lst;
    stat_to_linux(&kst, &lst);
    if (copy_to_user(ustatbuf, &lst, sizeof(lst)) != 0)
    {
        return ENO8_NULL_POINTER;
    }
    return 0;
}

/**
 * @brief newfstatat(dirfd, path, statbuf, flags)
 * @note `dirfd` 的支持范围与 `sys_openat` 一致：只认 `AT_FDCWD` 或绝对路径。
 * @note 支持 `AT_EMPTY_PATH`——path 为空串时退化成对 `dirfd` 本身做 fstat，
 *   代价只是多一个分支，musl 的 `fstat()` 在某些实现路径上就是这么包装 `newfstatat` 的。
 */
static long sys_newfstatat(int dirfd, const char *upath, struct linux_stat *ustatbuf, int flags)
{
    char kpath[VFS_PATH_MAX];
    long path_len = strncpy_from_user(kpath, upath, sizeof(kpath));
    if (path_len < 0)
    {
        return path_len;
    }

    stat_t kst;
    int ret;

    if ((flags & AT_EMPTY_PATH) && path_len == 0)
    {
        file_t *f = proc_fd_get(dirfd);
        if (!f)
        {
            return ENO19_BAD_FD;
        }
        if (f->f_kind == FILE_KIND_VFS)
        {
            vfs_lock();
            ret = vfs_fstat(f, &kst);
            vfs_unlock();
        }
        else
        {
            ret = fill_stat_nonvfs(f, &kst);
        }
    }
    else
    {
        if (dirfd != AT_FDCWD && kpath[0] != '/')
        {
            return ENO19_BAD_FD;
        }
        vfs_lock();
        ret = vfs_stat(kpath, &kst);
        vfs_unlock();
    }

    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }

    struct linux_stat lst;
    stat_to_linux(&kst, &lst);
    if (copy_to_user(ustatbuf, &lst, sizeof(lst)) != 0)
    {
        return ENO8_NULL_POINTER;
    }
    return 0;
}

/**
 * @brief getdents64(fd, buf, len)：读取目录项
 * @details 中转缓冲同样必须 kmalloc——内核栈只有 1 页，扛不住调用方给的 len。
 *   一次最多搬 SYS_RW_BUF_SIZE 字节；调用方给的 len 更大时只填这么多，
 *   getdents64 本来就允许"返回的字节数少于缓冲区容量"，调用方会继续循环调用。
 */
static long sys_getdents64(int fd, void *ubuf, uint64_t len)
{
    file_t *f = proc_fd_get(fd);
    if (!f)
    {
        return ENO19_BAD_FD;
    }

    uint64_t n = len;
    if (n > SYS_RW_BUF_SIZE)
    {
        n = SYS_RW_BUF_SIZE;
    }

    char *kbuf = kmalloc(SYS_RW_BUF_SIZE);
    if (!kbuf)
    {
        return ENO1_NOMORE_MEM;
    }

    vfs_lock();
    int ret = vfs_getdents(f, kbuf, (size_t)n);
    vfs_unlock();

    if (ret > 0 && copy_to_user(ubuf, kbuf, (uint64_t)ret) != 0)
    {
        ret = ENO8_NULL_POINTER;
    }

    kfree(kbuf);
    return ret;
}

/* ============================================================
 * 路径操作 syscall
 *
 * 都是同构薄壳：strncpy_from_user → 校验 dirfd → vfs_lock → 已有 vfs_* → vfs_unlock。
 * dirfd 的支持范围与 sys_openat 完全一致（只认 AT_FDCWD 或绝对路径），理由见
 * sys_openat 的函数级注释，不再重复。
 * ============================================================ */

/* 把用户路径拷进内核并校验 dirfd；成功返回 0，失败返回负 ENO*（调用方直接透传）。 */
static long fetch_path_at(int dirfd, const char *upath, char *kpath, size_t cap)
{
    long path_len = strncpy_from_user(kpath, upath, cap);
    if (path_len < 0)
    {
        return path_len;
    }
    if (dirfd != AT_FDCWD && kpath[0] != '/')
    {
        return ENO19_BAD_FD;
    }
    return 0;
}

static long sys_mkdirat(int dirfd, const char *upath, int mode)
{
    char kpath[VFS_PATH_MAX];
    long ret = fetch_path_at(dirfd, upath, kpath, sizeof(kpath));
    if (ret < 0)
    {
        return ret;
    }

    vfs_lock();
    ret = vfs_mkdir(kpath, (mode_t)mode);
    vfs_unlock();
    return ret;
}

/* unlinkat(dirfd, path, flags)：AT_REMOVEDIR 时等价于 rmdir，否则等价于 unlink。
 * BusyBox 的 rmdir / rm -r 靠这个标志，必须实现。 */
static long sys_unlinkat(int dirfd, const char *upath, int flags)
{
    char kpath[VFS_PATH_MAX];
    long ret = fetch_path_at(dirfd, upath, kpath, sizeof(kpath));
    if (ret < 0)
    {
        return ret;
    }

    vfs_lock();
    ret = (flags & AT_REMOVEDIR) ? vfs_rmdir(kpath) : vfs_unlink(kpath);
    vfs_unlock();
    return ret;
}

static long sys_renameat(int olddirfd, const char *uoldpath,
                         int newdirfd, const char *unewpath)
{
    /* 两个 VFS_PATH_MAX 缓冲共 512 字节，在 1 页内核栈里可以接受 */
    char koldpath[VFS_PATH_MAX];
    char knewpath[VFS_PATH_MAX];

    long ret = fetch_path_at(olddirfd, uoldpath, koldpath, sizeof(koldpath));
    if (ret < 0)
    {
        return ret;
    }
    ret = fetch_path_at(newdirfd, unewpath, knewpath, sizeof(knewpath));
    if (ret < 0)
    {
        return ret;
    }

    vfs_lock();
    ret = vfs_rename(koldpath, knewpath);
    vfs_unlock();
    return ret;
}

static long sys_chdir(const char *upath)
{
    char kpath[VFS_PATH_MAX];
    long path_len = strncpy_from_user(kpath, upath, sizeof(kpath));
    if (path_len < 0)
    {
        return path_len;
    }

    vfs_lock();
    long ret = vfs_chdir(kpath);
    vfs_unlock();
    return ret;
}

/**
 * @brief getcwd(buf, size)
 * @note **返回值不是 0**：Linux 的 getcwd 成功时返回写入缓冲区的字节数（含结尾 '\0'），
 *   musl 靠这个判断是否成功。而内核内部的 vfs_getcwd 返回的是 ENO0_NO_ERROR，
 *   所以这里要自己 strlen 后换算——直接透传 vfs_getcwd 的返回值是错的。
 */
static long sys_getcwd(char *ubuf, uint64_t size)
{
    if (size == 0)
    {
        return ENO6_INVAL_PARAM;
    }

    uint64_t n = size;
    if (n > VFS_PATH_MAX)
    {
        n = VFS_PATH_MAX;
    }

    char kpath[VFS_PATH_MAX];
    vfs_lock();
    int ret = vfs_getcwd(kpath, (size_t)n);
    vfs_unlock();
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }

    uint64_t len = (uint64_t)strlen(kpath) + 1; /* 含结尾 '\0' */
    if (copy_to_user(ubuf, kpath, len) != 0)
    {
        return ENO8_NULL_POINTER;
    }
    return (long)len;
}

static long sys_ftruncate(int fd, long length)
{
    if (length < 0)
    {
        return ENO6_INVAL_PARAM;
    }

    file_t *f = proc_fd_get(fd);
    if (!f)
    {
        return ENO19_BAD_FD;
    }

    vfs_lock();
    long ret = vfs_ftruncate(f, (uint64_t)length);
    vfs_unlock();
    return ret;
}

static long sys_close(int fd)
{
    return proc_fd_close(fd);
}

/**
 * @brief pipe2(pipefd, flags)：创建一对匿名管道 fd，`pipefd[0]` 读端、`pipefd[1]` 写端
 * @note flags 白名单：只接受 `O_CLOEXEC | O_NONBLOCK`，其余一律 `-EINVAL`——
 *   与 `sys_fcntl` 的 `F_SETFL` 只放行两个 flag 的处理保持一致的风格。
 */
static long sys_pipe2(int *ufd, int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
    {
        return ENO6_INVAL_PARAM;
    }

    file_t *rf = NULL;
    file_t *wf = NULL;
    int ret = pipe_alloc(&rf, &wf);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }

    if (flags & O_NONBLOCK)
    {
        rf->f_mode |= O_NONBLOCK;
        wf->f_mode |= O_NONBLOCK;
    }

    int fd0 = proc_fd_alloc();
    if (fd0 < 0)
    {
        vfs_close(rf);
        vfs_close(wf);
        return ENO18_TOO_MANY_FILES;
    }
    proc_fd_install(fd0, rf);
    if (flags & O_CLOEXEC)
    {
        proc_fd_set_flags(fd0, FD_CLOEXEC);
    }

    /* fd0 必须先装好才能要第二个 fd——proc_fd_alloc() 是"找最小空闲槽"，装之前
     * 槽位还是空的，连续调两次会拿到同一个 fd 号，两个 file 装进同一个槽，
     * 写端指针直接泄漏 */
    int fd1 = proc_fd_alloc();
    if (fd1 < 0)
    {
        proc_fd_close(fd0); /* 连带关掉已装入的 rf */
        vfs_close(wf);      /* wf 还没装进任何 fd，直接关 */
        return ENO18_TOO_MANY_FILES;
    }
    proc_fd_install(fd1, wf);
    if (flags & O_CLOEXEC)
    {
        proc_fd_set_flags(fd1, FD_CLOEXEC);
    }

    int fds[2] = {fd0, fd1};
    if (copy_to_user(ufd, fds, sizeof(fds)) != 0)
    {
        proc_fd_close(fd0);
        proc_fd_close(fd1);
        return ENO8_NULL_POINTER;
    }

    return 0;
}

/* 把 f 复制到 >= from 的最小空闲 fd（dup / fcntl F_DUPFD 共用）。
 * cloexec 为 true 时给新 fd 置上 FD_CLOEXEC（F_DUPFD_CLOEXEC 用）。 */
static long dup_fd_from(file_t *f, int from, bool cloexec)
{
    int newfd = proc_fd_alloc_from(from);
    if (newfd < 0)
    {
        return newfd; /* 透传 EMFILE / EINVAL */
    }

    f->f_count++;
    proc_fd_install(newfd, f);
    if (cloexec)
    {
        proc_fd_set_flags(newfd, FD_CLOEXEC);
    }
    return newfd;
}

/* dup(oldfd)：把 oldfd 复制到当前进程最小空闲 fd，两者指向同一个 file_t（共享偏移）。
 * 按 POSIX，dup 出来的新 fd 不继承 FD_CLOEXEC（该标志是 per-fd 而非 per-file）。 */
static long sys_dup(int oldfd)
{
    file_t *f = proc_fd_get(oldfd);
    if (!f)
    {
        return ENO19_BAD_FD; /* oldfd 无效 */
    }
    return dup_fd_from(f, 0, false);
}

/**
 * @brief fcntl(fd, cmd, arg)
 * @note F_GETFL 返回的是 file->f_mode（打开时传入的 O_* 组合）。F_SETFL 按 Linux 语义
 *   只允许改 O_APPEND/O_NONBLOCK，其余位（尤其是访问模式 O_ACCMODE）静默忽略——
 *   否则用户态可以把一个只读 fd 改成可写，绕过 open 时的权限判定。
 */
static long sys_fcntl(int fd, int cmd, long arg)
{
    file_t *f = proc_fd_get(fd);
    if (!f)
    {
        return ENO19_BAD_FD;
    }

    switch (cmd)
    {
    case F_DUPFD:
        return dup_fd_from(f, (int)arg, false);
    case F_DUPFD_CLOEXEC:
        return dup_fd_from(f, (int)arg, true);
    case F_GETFD:
        return (long)proc_fd_get_flags(fd);
    case F_SETFD:
        /* 目前只有 FD_CLOEXEC 一位有意义，其余位丢弃 */
        proc_fd_set_flags(fd, (uint8_t)(arg & FD_CLOEXEC));
        return 0;
    case F_GETFL:
        return (long)f->f_mode;
    case F_SETFL:
    {
        int mutable_bits = O_APPEND | O_NONBLOCK;
        f->f_mode = (f->f_mode & ~mutable_bits) | ((int)arg & mutable_bits);
        return 0;
    }
    default:
        return ENO6_INVAL_PARAM;
    }
}

/**
 * @brief ioctl(fd, cmd, arg)
 * @note 只服务 TTY（`tty_from_file` 判定，不是简单看 `f_kind==FILE_KIND_DEVICE`——
 *   以后加了 `/dev/null` 之类的其它字符设备，那些 file 的 `f_private` 不是
 *   `tty_t*`，用 `f_kind` 单独判断会把它们的 `f_private` 错当 `tty_t*` 解释）。
 *   非 TTY 的 fd 一律 `-ENOTTY`——BusyBox/ash 靠这个错码判断"是不是在终端里跑"。
 * @note `TCSETS`/`TCSETSW`/`TCSETSF` 当前行为完全相同：不做 drain/flush，
 *   TTY 缓冲很小，区别在交互上不可见，是有意的简化。改 `tio` 要持 `tty->lock`——
 *   `tty_input_push` 在中断里读同一份 `c_lflag`。
 * @note 不清空输入缓冲：ash 每次 fork 前后都会 set 一遍 termios，
 *   清缓冲会把用户已经敲进去、还没读走的字符吃掉。
 */
static long sys_ioctl(int fd, unsigned long cmd, unsigned long arg)
{
    file_t *f = proc_fd_get(fd);
    if (!f)
    {
        return ENO19_BAD_FD;
    }
    tty_t *tty = tty_from_file(f);
    if (!tty)
    {
        return ENO23_NOT_TTY;
    }

    switch (cmd)
    {
    case TCGETS:
        if (copy_to_user((void *)arg, &tty->tio, sizeof(tty->tio)) != 0)
        {
            return ENO8_NULL_POINTER;
        }
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
    {
        struct linux_termios kt;
        if (copy_from_user(&kt, (const void *)arg, sizeof(kt)) != 0)
        {
            return ENO8_NULL_POINTER;
        }
        spinlock_acquire(&tty->lock);
        tty->tio = kt;
        spinlock_release(&tty->lock);
        return 0;
    }
    case TIOCGWINSZ:
        if (copy_to_user((void *)arg, &tty->ws, sizeof(tty->ws)) != 0)
        {
            return ENO8_NULL_POINTER;
        }
        return 0;
    case TIOCSWINSZ:
        /* 串口没有真实窗口尺寸，收下即可 */
        return 0;
    default:
        return ENO23_NOT_TTY;
    }
}

/* dup3(oldfd, newfd, flags)：把 oldfd 复制到指定的 newfd（若已打开则先关掉）。
 * 用户态 dup2 = dup3(o,n,0)，且 dup2 在库层处理 oldfd==newfd，故内核 dup3 对相等直接判非法。 */
static long sys_dup3(int oldfd, int newfd, int flags)
{
    file_t *f = proc_fd_get(oldfd);
    if (!f)
    {
        return ENO19_BAD_FD; /* oldfd 无效 */
    }
    if (oldfd == newfd || newfd < 0 || newfd >= NOFILE)
    {
        return ENO6_INVAL_PARAM; /* dup3 要求 oldfd != newfd；newfd 越界非法 */
    }
    if (proc_fd_get(newfd))
    {
        /* newfd 已占用：先关闭。oldfd 仍持有 f 的引用，即便二者是同一 file_t 也不会被提前释放 */
        proc_fd_close(newfd);
    }
    f->f_count++;
    proc_fd_install(newfd, f);
    /* dup3 的 flags 里只有 O_CLOEXEC 有意义（此前是 (void)flags 丢弃，现已接上）*/
    if (flags & O_CLOEXEC)
    {
        proc_fd_set_flags(newfd, FD_CLOEXEC);
    }
    return newfd;
}

static long sys_exit(int code)
{
    do_exit((int16_t)code);
    return 0; /* unreachable */
}

static long sys_getpid(void)
{
    return proc_get_current()->proc_pid;
}

static long sys_getppid(void)
{
    pcb_t *p = proc_get_current()->proc_parent;
    return p ? p->proc_pid : 0;
}

static long sys_clone(intstkf_t *sp)
{
    return do_fork(0, sp->x2_sp, sp);
}

/* a0=path, a1=argv, a2=envp（本阶段忽略 argv/envp）。成功后 sp 已被改写为进入新程序的帧，
 * 本函数返回 0；trap.c 会把 0 写回 a0（新程序 _start 不读 argc，无害）。失败返回负 ENO*。 */
static long sys_execve(intstkf_t *sp)
{
    return do_exec(sp, (const char *)sp->x10_a0);
}

static long sys_wait4(int pid, int *ustatus, int options, void *rusage)
{
    int kstatus = 0;
    int16_t ret = do_wait((int16_t)pid, &kstatus);
    if (ret > 0 && ustatus)
    {
        if (copy_to_user(ustatus, &kstatus, sizeof(kstatus)) != 0)
        {
            return ENO8_NULL_POINTER;
        }
    }
    
    return ret;
}

static inline virAddr_t syscall_round_up_page(virAddr_t va)
{
    return (va + PGSIZE - 1) & ~(virAddr_t)(PGSIZE - 1);
}

/**
 * @name sys_brk
 * @brief 查询或调整进程堆顶
 * @param[in] addr 期望的新堆顶；0 表示只查询
 * @return 生效后的 brk 值
 * @details 成功返回新 brk，失败返回**旧 brk**——内核侧的 brk 不返回负 errno。
 *   musl 的 __expand_heap 判断成功的方式是"返回值 >= 请求的 addr"，返回
 *   -ENOMEM 会被它当成一个合法的天文数字堆顶，随后立刻踩空。
 *
 *   扩张只改 brk_current 这一个整数，物理页留给缺页处理懒分配；收缩则必须真正
 *   解映射并归还物理页，否则 malloc 每次 free 大块后堆都不缩，6 MB 撑不了几轮。
 */
static long sys_brk(virAddr_t addr)
{
    mm_t *mm = proc_get_current()->proc_mm;
    if (mm == NULL || mm->brk_start == 0)
    {
        return 0;
    }

    virAddr_t old = mm->brk_current;
    if (addr == 0 || addr < mm->brk_start || addr > mm->brk_start + USER_HEAP_MAX)
    {
        return (long)old;
    }

    virAddr_t new_page_end = syscall_round_up_page(addr);
    virAddr_t old_page_end = syscall_round_up_page(old);
    if (new_page_end < old_page_end)
    {
        vmm_unmap_range(mm, new_page_end, old_page_end);
    }
    mm->brk_current = addr;
    return (long)addr;
}

/* PROT_NONE（0）在本项目没有对应表示——VMA 存在即可访问，没有"存在但不可访问"
 * 这一档，第一版按 VMP_R 处理。@TODO 需要真实 PROT_NONE 语义时补 VMA 级别的标志。 */
static pgprot_t prot_to_vmp(int prot)
{
    pgprot_t flag = 0;
    if (prot & PROT_READ)
    {
        flag |= VMP_R;
    }
    if (prot & PROT_WRITE)
    {
        flag |= VMP_W;
    }
    if (prot & PROT_EXEC)
    {
        flag |= VMP_X;
    }
    if (flag == 0)
    {
        flag = VMP_R;
    }
    return flag;
}

/**
 * @name sys_mmap
 * @brief 匿名私有映射，返回一段新的用户虚拟地址
 * @param[in] addr   建议地址，本实现忽略（不支持 MAP_FIXED）
 * @param[in] len    映射长度，向上取整到页
 * @param[in] prot   PROT_READ/WRITE/EXEC 组合
 * @param[in] flags  必须含 MAP_ANONYMOUS，不能含 MAP_FIXED
 * @param[in] fd     必须为 -1（不支持文件映射）
 * @param[in] offset 忽略
 * @retval <0 -errno
 * @return 映射区起始地址
 * @note 与 brk 相反，这里失败返回 -errno 而不是 MAP_FAILED——MAP_FAILED((void*)-1)
 *   是 libc 层的约定，内核返回 -1 会被 musl 解读成 errno=EPERM 且地址有效。
 *   只建 VMA 不建映射，物理页由缺页处理懒分配。
 */
static long sys_mmap(virAddr_t addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
    (void)addr;
    (void)offset;

    mm_t *mm = proc_get_current()->proc_mm;
    if (mm == NULL || len == 0)
    {
        return ENO6_INVAL_PARAM;
    }
    if ((flags & MAP_ANONYMOUS) == 0)
    {
        return ENO20_NOSYS;
    }
    if (flags & MAP_FIXED)
    {
        return ENO6_INVAL_PARAM; /* @TODO musl 若真的需要，再补 MAP_FIXED */
    }
    if (fd != -1)
    {
        return ENO6_INVAL_PARAM;
    }

    len = syscall_round_up_page(len);
    virAddr_t va = vmm_mmap_find_free_area(mm, len);
    if (va == 0)
    {
        return ENO1_NOMORE_MEM;
    }
    vma_t *vma = vmm_vma_create(va, va + len, prot_to_vmp(prot));
    if (vma == NULL)
    {
        return ENO1_NOMORE_MEM;
    }
    vmm_vma_insert(mm, vma);
    return (long)va;
}

/**
 * @name sys_munmap
 * @brief 解除 [addr, addr+len) 的映射，回收物理页并调整/删除/分裂相关 VMA
 * @param[in] addr 起始地址，必须页对齐
 * @param[in] len  长度，向上取整到页
 * @retval 0 成功
 * @retval <0 -errno
 * @details 请求区间可以横跨多个 VMA，所以外层循环每轮重新定位；与单个 VMA 的
 *   四种关系分别处理：
 *   1. 完全覆盖 → 摘链并销毁整个 VMA；
 *   2. 截断头部 → vm_start = 区间末尾；
 *   3. 截断尾部 → vm_end = 区间开头；
 *   4. 中间打洞 → 原 VMA 收尾，另建一条 [洞末尾, 原 vm_end)。
 *
 *   情形 4 的新 vma_t 必须在动原 VMA 之前分配好，分配失败直接返回，
 *   不留"洞已打、VMA 还没分裂"的半截状态。
 * @note 打到堆 VMA 上第一版直接拒绝：本项目的堆边界由 brk_current 单独表达，
 *   截断堆 VMA 会与它冲突。musl 与 BusyBox 都不会 munmap 堆区。
 */
static long sys_munmap(virAddr_t addr, uint64_t len)
{
    mm_t *mm = proc_get_current()->proc_mm;
    if (mm == NULL || len == 0 || (addr % PGSIZE) != 0)
    {
        return ENO6_INVAL_PARAM;
    }

    virAddr_t start = addr;
    virAddr_t end = syscall_round_up_page(addr + len);
    if (end <= start)
    {
        return ENO6_INVAL_PARAM;
    }

    /* 堆 VMA 的拒绝必须在动手之前判掉：请求区间可以横跨多条 VMA，
     * 边解边判的话前面几条已经解完了才发现要拒绝，留下半截状态 */
    struct list_head *scan;
    list_for_each(scan, &mm->mmap_list)
    {
        vma_t *cur = list_entry(scan, vma_t, vma_list_linker);
        if ((cur->vm_flag & VMA_HEAP) && cur->vm_start < end && start < cur->vm_end)
        {
            return ENO6_INVAL_PARAM;
        }
    }

    while (start < end)
    {
        vma_t *vma = vmm_vma_get(mm, start);
        if (vma == NULL)
        {
            /* 这一页本来就没映射：跳到下一条起始地址 >= start 的 VMA 继续 */
            virAddr_t next = 0;
            struct list_head *pos;
            list_for_each(pos, &mm->mmap_list)
            {
                vma_t *cur = list_entry(pos, vma_t, vma_list_linker);
                if (cur->vm_start > start)
                {
                    next = cur->vm_start;
                    break;
                }
            }
            if (next == 0 || next >= end)
            {
                break;
            }
            start = next;
            continue;
        }

        virAddr_t seg_end = vma->vm_end < end ? vma->vm_end : end;

        if (start > vma->vm_start && seg_end < vma->vm_end)
        {
            vma_t *tail = vmm_vma_create(seg_end, vma->vm_end, vma->vm_flag);
            if (tail == NULL)
            {
                return ENO1_NOMORE_MEM;
            }
            vmm_unmap_range(mm, start, seg_end);
            vma->vm_end = start;
            mm->last_access = NULL;
            vmm_vma_insert(mm, tail);
        }
        else
        {
            vmm_unmap_range(mm, start, seg_end);
            if (start <= vma->vm_start && seg_end >= vma->vm_end)
            {
                list_del(&vma->vma_list_linker);
                mm->map_count--;
                mm->last_access = NULL;
                vmm_vma_destroy(vma);
            }
            else if (start <= vma->vm_start)
            {
                vma->vm_start = seg_end;
                mm->last_access = NULL;
            }
            else
            {
                vma->vm_end = start;
                mm->last_access = NULL;
            }
        }

        start = seg_end;
    }

    return ENO0_NO_ERROR;
}

long syscall_dispatch(intstkf_t *sp)
{
    switch (sp->x17_a7) /* a7中存放了系统调用号 */
    {
    case __NR_write:
        return sys_write((int)sp->x10_a0, (const char *)sp->x11_a1, sp->x12_a2);
    case __NR_read:
        return sys_read((int)sp->x10_a0, (char *)sp->x11_a1, sp->x12_a2);
    case __NR_writev:
        return sys_writev((int)sp->x10_a0, (const struct iovec *)sp->x11_a1, (int)sp->x12_a2);
    case __NR_readv:
        return sys_readv((int)sp->x10_a0, (const struct iovec *)sp->x11_a1, (int)sp->x12_a2);
    case __NR_openat:
        return sys_openat((int)sp->x10_a0, (const char *)sp->x11_a1, (int)sp->x12_a2, (int)sp->x13_a3);
    case __NR_lseek:
        return sys_lseek((int)sp->x10_a0, (long)sp->x11_a1, (int)sp->x12_a2);
    case __NR_getdents64:
        return sys_getdents64((int)sp->x10_a0, (void *)sp->x11_a1, sp->x12_a2);
    case __NR_fstat:
        return sys_fstat((int)sp->x10_a0, (struct linux_stat *)sp->x11_a1);
    case __NR_newfstatat:
        return sys_newfstatat((int)sp->x10_a0, (const char *)sp->x11_a1,
                               (struct linux_stat *)sp->x12_a2, (int)sp->x13_a3);
    case __NR_close:
        return sys_close((int)sp->x10_a0);
    case __NR_pipe2:
        return sys_pipe2((int *)sp->x10_a0, (int)sp->x11_a1);
    case __NR_mkdirat:
        return sys_mkdirat((int)sp->x10_a0, (const char *)sp->x11_a1, (int)sp->x12_a2);
    case __NR_unlinkat:
        return sys_unlinkat((int)sp->x10_a0, (const char *)sp->x11_a1, (int)sp->x12_a2);
    case __NR_renameat:
        return sys_renameat((int)sp->x10_a0, (const char *)sp->x11_a1,
                            (int)sp->x12_a2, (const char *)sp->x13_a3);
    case __NR_chdir:
        return sys_chdir((const char *)sp->x10_a0);
    case __NR_getcwd:
        return sys_getcwd((char *)sp->x10_a0, sp->x11_a1);
    case __NR_ftruncate:
        return sys_ftruncate((int)sp->x10_a0, (long)sp->x11_a1);
    case __NR_fcntl:
        return sys_fcntl((int)sp->x10_a0, (int)sp->x11_a1, (long)sp->x12_a2);
    case __NR_ioctl:
        return sys_ioctl((int)sp->x10_a0, (unsigned long)sp->x11_a1, (unsigned long)sp->x12_a2);
    case __NR_dup:
        return sys_dup((int)sp->x10_a0);
    case __NR_dup3:
        return sys_dup3((int)sp->x10_a0, (int)sp->x11_a1, (int)sp->x12_a2);
    case __NR_exit:
    case __NR_exit_group: /* 本阶段暂作 exit 别名，不区分线程组 */
        return sys_exit((int)sp->x10_a0);
    case __NR_getpid:
        return sys_getpid();
    case __NR_getppid:
        return sys_getppid();
    case __NR_clone:
        return sys_clone(sp);
    case __NR_execve:
        return sys_execve(sp);
    case __NR_wait4:
        return sys_wait4((int)sp->x10_a0, (int *)sp->x11_a1, (int)sp->x12_a2, (void *)sp->x13_a3);
    case __NR_brk:
        return sys_brk((virAddr_t)sp->x10_a0);
    case __NR_mmap:
        return sys_mmap((virAddr_t)sp->x10_a0, (uint64_t)sp->x11_a1, (int)sp->x12_a2,
                        (int)sp->x13_a3, (int)sp->x14_a4, (uint64_t)sp->x15_a5);
    case __NR_munmap:
        return sys_munmap((virAddr_t)sp->x10_a0, (uint64_t)sp->x11_a1);
    default:
        printf("syscall: unknown nr=%ld\n", sp->x17_a7);
        return ENO20_NOSYS;
    }
}
