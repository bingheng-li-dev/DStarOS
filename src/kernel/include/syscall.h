/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "trap.h"

/* Linux风格的系统调用号，NR为Number */
#define __NR_getcwd      17
#define __NR_set_tid_address 96
#define __NR_nanosleep      101
#define __NR_getitimer      102
#define __NR_setitimer      103
#define __NR_clock_settime  112
#define __NR_clock_gettime  113
#define __NR_clock_getres   114
#define __NR_clock_nanosleep 115
#define __NR_sched_yield    124
#define __NR_dup         23
#define __NR_dup3        24
#define __NR_fcntl       25
#define __NR_ioctl       29
#define __NR_mkdirat     34
#define __NR_unlinkat    35
/* riscv64 的 asm-generic ABI **没有 renameat(38)**（那是 __ARCH_WANT_RENAMEAT 的
 * 老架构才有的），只有 renameat2。musl 的 rename()/renameat() 都发这个号。 */
#define __NR_renameat2   276
#define __NR_ftruncate   46
#define __NR_chdir       49
#define __NR_openat      56
#define __NR_faccessat   48
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
#define __NR_times      153
#define __NR_setpgid    154
#define __NR_getpgid    155
#define __NR_uname      160
#define __NR_umask      166
#define __NR_prctl      167
#define __NR_gettimeofday 169
#define __NR_settimeofday 170
#define __NR_getpid     172
#define __NR_getppid    173
#define __NR_getuid     174
#define __NR_geteuid    175
#define __NR_getgid     176
#define __NR_getegid    177
#define __NR_gettid     178
#define __NR_brk        214
#define __NR_munmap     215
#define __NR_clone      220
#define __NR_execve     221
#define __NR_mmap       222
#define __NR_wait4      260

/* Linux riscv64 的 syscall 接口约定返回类型就是 long */
long syscall_dispatch(intstkf_t *sp);   /* a7=号, a0..a5=参, 返回值即写回 a0 */

#endif
