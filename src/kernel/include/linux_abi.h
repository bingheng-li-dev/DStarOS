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
#define DT_CHR      2
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

/* ============================================================
 * termios / ioctl（asm-generic，riscv64 用的就是这一套）
 * ============================================================ */

/* ioctl 命令号 */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414

/* c_iflag */
#define BRKINT  0x0002
#define ICRNL   0x0100
#define IXON    0x0400

/* c_oflag */
#define OPOST   0x0001
#define ONLCR   0x0004

/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define IEXTEN  0x8000

/* c_cc 下标 */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSTART  8
#define VSTOP   9
#define VSUSP   10

#define NCCS 19  /* struct linux_termios 的 c_cc 长度（不是 musl 的 32） */

/* struct termios（Linux 内核 asm-generic 版，36 字节，字段偏移固定，不可重排）。
 * musl 的 struct termios 是 60 字节（NCCS=32，尾部还有 __c_ispeed/__c_ospeed）——
 * 这是 Linux 上的正常行为，内核只按这 36 字节的形状读写，musl 自己截断/补齐，
 * 不是本内核要对齐 musl 的信号。 */
struct linux_termios
{
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS];
};
_Static_assert(sizeof(struct linux_termios) == 36, "linux_termios size must match Linux kernel ABI");

/* TIOCGWINSZ / TIOCSWINSZ 用的窗口尺寸，8 字节 */
struct winsize
{
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};
_Static_assert(sizeof(struct winsize) == 8, "winsize size must match Linux kernel ABI");

/* mmap 的 prot / flags（asm-generic，riscv64 上 MAP_ANONYMOUS = 0x20） */
#define PROT_NONE      0x0
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4

#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

#endif
