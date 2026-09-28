/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _DEBUG_H
#define _DEBUG_H

#define DEBUG 1

#if DEBUG

#define DEBUG_INIT_os_init 0

#define DEBUG_INIT_main 1
#if DEBUG_INIT_main
#define DEBUG_INIT_main_core0 1
#define DEBUG_INIT_main_core1 1
#define DEBUG_INIT_main_bothcore 0
#endif /* DEBUG_INIT_main */

#define DEBUG_TICK 0
#define DEBUG_INTSTACK 0
#define DEBUG_MMU_mm_init 0
#define DEBUG_MMU_mm_alloc 0
#define DEBUG_MMU_mm_dealloc 0
#define DEBUG_MMU_deleteAndReinsert 0
#define DEBUG_MMU_insertAndMerge 0
#define DEBUG_LOCK_irq_enable 0
#define DEBUG_PROC_idle 0   /* hart 1 的 idle 会一直空转，置 1 会刷屏 */
#define DEBUG_PROC_createFirstProcIdle 0
#define DEBUG_PROC_proc_init 0
#define DEBUG_PROC_do_fork 0
#define DEBUG_PROC_findProcByPid 0
#define DEBUG_PROC_allocNewProc 0
#define DEBUG_PROC_CTXSTK   0
#define DEBUG_PROC_init     0

#define DEBUG_VMM 1
#if DEBUG_VMM
#define DEBUG_VMM_page_fault_handler 0
#define DEBUG_VMM_self_test 0

#endif /* DEBUG_VMM */

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

/* PTE A/D 位实测探针：置 1 时在 hart0 初始化阶段（trap_init 之后）跑
 * vmm_probe_pte_ad()，判定本平台是硬件自动置位 A/D 还是软件管理。
 * 时钟置换算法依赖硬件自动置位，动手前用它确认，不要照规范假设。
 * 串口应看到 "pte_ad_probe: RESULT = ..."。 */
#define DEBUG_PTE_AD_PROBE 0

/* 上板 bring-up 诊断：置 1 时打印 MMU 开启所依赖的两条关键映射，并在 PC 真正
 * 落到高 VA 之后立刻吐一行。用来切开"vmm_init 卡住 / satp 写完跑飞 /
 * os_init_after_mmu_enable 早期挂掉"这三段——它们的表现都是"pmm inited 之后没声了"。
 * 主线在板子上跑通之后置 0。 */
#define DEBUG_BRINGUP 0

/* MMIO 映射探针：置 1 时在 trap_init() 之后打印设备区的页表项，并读若干身份寄存器。
 * 判据是**固定值**，不是状态位：VF2 看 UART 的 CTR(+0xfc) = 0x44570110、
 * SD 控制器 VERID 高 16 位 = 0x5342；QEMU 看 CLINT mtime 两次读数不同。
 * ⚠️ **不要拿 LSR.THRE = 1 当判据**：真串口在发送时 THRE/TEMT 都是 0（VF2 实测
 * LSR=0x00、USR=0x03 即 BUSY 且 TX FIFO 非空——固件 putc 塞进 FIFO 就返回，
 * 上一行 printf 还在发），只有 QEMU 那种瞬间发完的虚拟串口才恒为 1。
 * **只读，且避开有副作用的寄存器**。 */
#define DEBUG_MMIO_PROBE 0

/* 外部中断追踪：置 1 时每次 claim 到外部中断都打印 "extirq: irq N on cpu C (#K)"。
 * 用来确认 PLIC 路由与 context 编号：只应出现在 cpu 0 上，且 irq 等于 UART_IRQ。
 * 打字会刷屏，只在验证时打开。 */
#define DEBUG_EXT_IRQ 0

/* SD 卡探针（仅 VF2），置 1 时：
 * 1. trap_init() 之后切到 PIO、发 CMD16 确认卡处于传输态，读 LBA 0 与第一个分区的首扇区并打印（只读）；
 * 2. /sd 挂载后对比单块 / 多块传输：copy.img 逐块冷读、rootfs.img 多块冷读并再读一遍（缓存），
 *    冷读上次开机写下的 wsingle.bin / wmulti.bin，再分别以逐块 / 多块重写这两个 2 MB 文件（**会写卡**）。
 * 判据：CMD16 的 state=4；part1 的 fat=1；各 sdcheck 行 crc32 一致；sdwrite 行多块模式的 cmd25 远少于块数。 */
#define DEBUG_SDMMC_PROBE 0

/* SD 卡写通路回环测试（仅 VF2）：置 1 时在 trap_init() 之后，对 MBR 与第一个分区之间空洞里的
 * LBA 4096 做"读原值（必须全零）→ 写图案 → 读回比对 → 写回全零 → 读回确认"。
 * 不碰文件系统；分区表不是 MBR、分区起点不在其后、或原值不是全零时一律放弃不写。
 * 判据：串口出现 "sdwrite: PASS"。 */
#define DEBUG_SDMMC_WRITE_TEST 0

/* 启动跟踪：置 1 时在关键初始化步骤前后经 SBI 直接输出 "[trace] ..."。
 * 走固件而不走自有 UART 驱动：卡死若发生在串口驱动里，这些行仍然出得来，
 * 从而区分"代码卡住"与"串口发不出去"。 */
#define DEBUG_BOOT_TRACE 0

#define DEBUG_TRACK_LINE() printf("DEBUG_TRACK_LINE: %s:%d\n", __FILE__, __LINE__)

#endif /* DEBUG */

#if defined(DEBUG_BOOT_TRACE) && DEBUG_BOOT_TRACE
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
