#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "trap.h"

/* Linux风格的系统调用号，NR为Number */
#define __NR_getcwd      17
#define __NR_dup         23
#define __NR_dup3        24
#define __NR_fcntl       25
#define __NR_ioctl       29
#define __NR_mkdirat     34
#define __NR_unlinkat    35
#define __NR_renameat    38
#define __NR_ftruncate   46
#define __NR_chdir       49
#define __NR_openat      56
#define __NR_close       57
#define __NR_pipe2       59
#define __NR_getdents64  61
#define __NR_lseek       62
#define __NR_read        63
#define __NR_write       64
#define __NR_readv       65
#define __NR_writev      66
#define __NR_newfstatat  79
#define __NR_fstat       80
#define __NR_exit        93
#define __NR_exit_group  94
#define __NR_kill       129
#define __NR_tkill      130
#define __NR_rt_sigaction   134
#define __NR_rt_sigprocmask 135
#define __NR_rt_sigpending  136
#define __NR_rt_sigreturn   139
#define __NR_setpgid    154
#define __NR_getpgid    155
#define __NR_getpid     172
#define __NR_getppid    173
#define __NR_brk        214
#define __NR_munmap     215
#define __NR_clone      220
#define __NR_execve     221
#define __NR_mmap       222
#define __NR_wait4      260

/* Linux riscv64 的 syscall 接口约定返回类型就是 long */
long syscall_dispatch(intstkf_t *sp);   /* a7=号, a0..a5=参, 返回值即写回 a0 */

#endif
