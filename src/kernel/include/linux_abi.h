#ifndef _LINUX_ABI_H_
#define _LINUX_ABI_H_

#include <stdint.h>
#include <stddef.h>

/* U 态可见的 Linux riscv64 ABI 形状——syscall 参数/返回值里出现的常量与结构体布局，
 * 数值/偏移必须与真实 Linux 内核严格一致（musl/BusyBox 按这个二进制布局解析）。
 * 本文件只声明形状，不提供转换函数（stat_t → linux_stat 等转换见 syscall.c）。 */

/* ============================================================
 * *at 系列 dirfd 特殊值与标志位
 * ============================================================ */
#define AT_FDCWD            (-100)   /* 相对当前工作目录解析 */
#define AT_SYMLINK_NOFOLLOW  0x100   /* 不跟随符号链接（FAT 无符号链接，直接忽略）*/
#define AT_REMOVEDIR         0x200   /* unlinkat 删除目标是目录（等价 rmdir）*/
#define AT_EMPTY_PATH        0x1000  /* path 为空串时对 dirfd 本身操作 */

/* ============================================================
 * fcntl 命令与 fd 标志
 * ============================================================ */
#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_DUPFD_CLOEXEC  1030

#define FD_CLOEXEC       1

/* ============================================================
 * getdents64 目录项类型（d_type）
 * ============================================================ */
#define DT_UNKNOWN  0
#define DT_DIR      4
#define DT_REG      8

/* ============================================================
 * struct stat（Linux riscv64 asm-generic 版，128 字节，字段偏移固定，不可重排）
 * ============================================================ */
struct linux_stat
{
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    uint32_t __unused[2];
};

/* ============================================================
 * getdents64 变长目录项记录
 * ============================================================ */
struct linux_dirent64
{
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

/* ============================================================
 * readv/writev 的分散/聚集缓冲区描述符
 * ============================================================ */
struct iovec
{
    void   *iov_base;
    size_t  iov_len;
};

#endif
