#ifndef _ERRORCODE_H_
#define _ERRORCODE_H_

/* Linux riscv64 标准 errno 数值（asm-generic errno-base.h / errno.h），
 * syscall 边界要求失败时返回值落在 [-4095,-1]，其绝对值就是这里的数值——
 * musl/BusyBox 靠这个判断具体错误原因，数值不可自定义。 */
#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EBADF         9
#define ECHILD        10
#define EAGAIN        11
#define ENOMEM        12
#define EFAULT        14
#define EBUSY         16
#define EEXIST        17
#define EXDEV         18
#define ENODEV        19
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define EMFILE        24
#define EROFS         30
#define ENAMETOOLONG  36
#define ENOTTY        25
#define ENOSYS        38
#define ENOTEMPTY     39
#define ESPIPE        29
#define EPIPE         32

/** 内核内部错误码——名字保留历史命名（含数字后缀 ENOx，由于历史原因，现已不代表实际数值）
 * 值统一改写为对应的 * Linux errno 负值，
 * 使 syscall_dispatch 的返回值无需二次翻译即可直接作为 ABI 返回值。
 * 多个 ENO* 映射到同一个 Linux errno 是允许的（Linux 内部同样如此）。 */
#define ENO0_NO_ERROR 0
#define ENO1_NOMORE_MEM (-ENOMEM)
#define ENO2_ALLOCPROC_FAILED (-ENOMEM) /* PCB 分配失败，无专用 Linux 码，按语义就近 */
#define ENO3_NOFREE_PID (-EAGAIN)       /* pid 空间耗尽，对应 Linux fork() 的 EAGAIN */
#define ENO4_BUSY (-EBUSY)              /* 设备/文件正忙 */
#define ENO5_NOSUCH_ENTRY (-ENOENT)     /* 没有那个文件或目录 */
#define ENO6_INVAL_PARAM (-EINVAL)      /* 参数非法 */
#define ENO7_EXISTS (-EEXIST)           /* 文件/目录已存在 */

#define ENO8_NULL_POINTER (-EFAULT)     /* 空指针 */

/* 文件系统 VFS 专用错误码 */
#define ENO9_NOT_DIR        (-ENOTDIR)      /* 路径中某分量不是目录 */
#define ENO10_IS_DIR        (-EISDIR)       /* 目标是目录，但该操作不适用于目录 */
#define ENO11_NAME_TOO_LONG (-ENAMETOOLONG) /* 文件名或路径名过长 */
#define ENO12_NOT_EMPTY     (-ENOTEMPTY)    /* 目录非空，无法删除 */
#define ENO13_NO_FS         (-ENODEV)       /* 没有已挂载的根文件系统 */
#define ENO14_CROSS_DEV     (-EXDEV)        /* 跨挂载点重命名/移动 */
#define ENO15_READ_ONLY     (-EROFS)        /* 文件系统只读 */
#define ENO16_PERM          (-EPERM)        /* 操作不被允许 */
#define ENO17_NO_CHILD      (-ECHILD)       /* 没有子进程可等待 */

#define ENO18_TOO_MANY_FILES (-EMFILE) /* fd表满了 */

#define ENO19_BAD_FD (-EBADF) /* fd 无效/未打开 */
#define ENO20_NOSYS  (-ENOSYS) /* 未知 syscall 号 */

#define ENO21_ILLEGAL_SEEK (-ESPIPE) /* 对不支持 seek 的 file（管道/设备）做 lseek */
#define ENO22_BROKEN_PIPE  (-EPIPE)  /* 向读端已全部关闭的管道写入 */
#define ENO23_NOT_TTY      (-ENOTTY) /* 对非 TTY 的 fd 调 ioctl(TCGETS/...) */

#define ENO25_NO_SUCH_PROC (-ESRCH)  /* kill 的目标 pid / 进程组不存在 */
#define ENO26_INTERRUPTED  (-EINTR)  /* 阻塞的 syscall 被信号打断且不重启 */

/* 内核内部专用：阻塞循环被信号打断时返回它，由 signal_handle_pending() 统一翻译成
 * "重启该 syscall"或 ENO26_INTERRUPTED。**它永远不会出现在 syscall 的返回值里**，
 * 数值取 Linux 的 ERESTARTSYS(512)，落在合法 errno 区间 [-4095,-1] 之外正是为了
 * 让"不小心漏翻译"变成一眼可见的错误。 */
#define ERESTARTSYS        512
#define ENO24_RESTARTSYS   (-ERESTARTSYS)

#endif
