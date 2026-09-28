/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _DEBUG_H
#define _DEBUG_H

/* 每次进入 trap 打印整个 trap 帧 */
#define DEBUG_INTSTACK 0
/* 编进 print_ctx_stk()，供 GDB 里手动调用查看被切走任务的上下文 */
#define DEBUG_PROC_CTXSTK 0
/* 写时复制缺页：打印原地恢复写权限 / 复制新页两种路径 */
#define DEBUG_COW 0

/* 回归套件选择器：一次只选一套，scripts/regress_all.sh 与 tools/build_vf2_suites.sh 自动切换。
 * SUITE_NONE 是交付形态，PID 1 变身 /sbin/init；内核态套件跑完直接关机，
 * 用户态套件由 init fork 出测试程序并收割，没有子进程之后关机。 */
#define SUITE_NONE      0
#define SUITE_SCHED     1   /* 内核态：调度类、信号量、等待队列、管道 */
#define SUITE_SLAB      2   /* 内核态：slab 分配器 */
#define SUITE_DCACHE    3   /* 内核态：LRU 目录项缓存 */
#define SUITE_VFS       4   /* 内核态：VFS/FatFS、getcwd、挂载 */
#define SUITE_FILE      5   /* filetest：POSIX 文件 syscall，含双进程并发 */
#define SUITE_PIPE      6   /* pipetest：管道 */
#define SUITE_TTY       7   /* ttytest：行规范与 termios，输入由 regress.sh 经 QEMU stdin 喂入 */
#define SUITE_MEM       8   /* memtest：brk/mmap/munmap 与 COW */
#define SUITE_EXEC      9   /* exectest：dup 与 execve */
#define SUITE_SIG       10  /* sigtest：信号 */
#define SUITE_TIME      11  /* timetest：时间与杂项 syscall、execve 传 argv/envp */
#define SUITE_SEG       12  /* segtest：相邻 PT_LOAD 段共用一页 */
#define SUITE_WAIT      13  /* waittest：wait4 的 pid 选择与 WNOHANG、F_DUPFD */
#define SUITE_TRAP      14  /* trapkill：U 态同步异常只杀该进程 */
#define SUITE_MUSL      15  /* mhello：musl 启动路径 */
#define SUITE_MSYS      16  /* msyscheck：以 musl 为客户的 syscall 覆盖 */
#define SUITE_MROOT     17  /* mrootfs：rootfs 镜像内容可读，需先 make rootfs */
#define SUITE_MFP       18  /* mfptest：浮点上下文 */
#define SUITE_BB        19  /* BusyBox 冒烟命令串 */
#define SUITE_BB_SH     20  /* 不经 /sbin/init，直接起 BusyBox 交互 shell；不在回归里 */
#define SUITE_FORK_WAIT 21  /* fork_wait：clone/wait4 接线；不在回归里 */

#define DEBUG_SUITE SUITE_NONE

/* 以下为上板诊断，判据串见各函数注释 */

/* trap_init 之后实测硬件是否自动置位 PTE 的 A/D 位，看 "pte_ad_probe: RESULT = ..." */
#define DEBUG_PTE_AD_PROBE 0
/* 打印 MMU 开启依赖的两条映射，PC 到达高 VA 后立刻吐一行，切开"pmm inited 之后没声了"的三种原因 */
#define DEBUG_BRINGUP 0
/* trap_init 之后打印设备区页表项并读身份寄存器；判据是固定值，不要拿 LSR.THRE 当判据 */
#define DEBUG_MMIO_PROBE 0
/* 每次 claim 到外部中断打印 "extirq: irq N on cpu C (#K)"，打字会刷屏 */
#define DEBUG_EXT_IRQ 0
/* 仅 VF2：SD 读通路逐步探测，/sd 挂载后对比单块 / 多块读写并核对 CRC（会写卡） */
#define DEBUG_SDMMC_PROBE 0
/* 仅 VF2：对分区前空洞里的 LBA 4096 做写读回环，看 "sdwrite: PASS" */
#define DEBUG_SDMMC_WRITE_TEST 0
/* 关键初始化步骤前后经 SBI 直接输出 "[trace] ..."，不依赖自有串口驱动 */
#define DEBUG_BOOT_TRACE 0

#if DEBUG_BOOT_TRACE
#include "sbi.h"
#include <stdint.h>
#define BOOT_TRACE(msg) do { for (const char *bt_p_ = "[trace] " msg "\n"; *bt_p_ != '\0'; bt_p_++) { sbi_console_putchar((int)*bt_p_); } } while (0)
extern int boot_trace_armed;
static inline void boot_trace_hex(const char *label, uint64_t v)
{
    for (const char *p = "[trace] "; *p != '\0'; p++)
    {
        sbi_console_putchar((int)*p);
    }
    for (const char *p = label; *p != '\0'; p++)
    {
        sbi_console_putchar((int)*p);
    }
    sbi_console_putchar('=');
    for (int s = 60; s >= 0; s -= 4)
    {
        sbi_console_putchar((int)"0123456789abcdef"[(v >> s) & 0xf]);
    }
    sbi_console_putchar('\n');
}
#else
#define BOOT_TRACE(msg) do { } while (0)
#endif

#endif /* _DEBUG_H */
