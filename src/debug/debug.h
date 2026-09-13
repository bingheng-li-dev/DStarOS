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

/* 调度器/同步回归测试：置 1 时 init 进程改为运行 run_sched_tests() 再关机，
 * 不再启动用户程序；置 0 走正常的用户程序路径。测试代码在 src/debug/sched_test.c
 * 等文件，入口 run_sched_tests()。 */
#define DEBUG_SCHED_TEST 0

/* slab 分配器内核态自检：置 1 时 init 进程改为运行 run_slab_tests()（src/debug/slab_test.c）
 * 再关机，不启动用户程序。覆盖基本分配/复用、通用尺寸类 kmalloc 往返、碎片化下的
 * partial 分档效果、空页回收与 slab_reclaim_all()。
 * 串口应看到 "=== slabtest done: N pass  0 fail ==="。 */
#define DEBUG_SLAB_TEST 0

/* 目录项缓存（LRU dcache）内核态自检：置 1 时 init 改为运行 run_dcache_tests()
 * （src/debug/dcache_test.c）再关机，不启动用户程序。覆盖基本命中、LRU 复活与入队
 * 出队配对、缓存叶子对祖先链的钉住效果、unlink 逐出、目录改名后子树仍可用、
 * 水位线压制与内存归还、进程退出归还 cwd 引用。
 * 串口应看到 "=== dcachetest done: N pass  0 fail ==="。 */
#define DEBUG_DCACHE_TEST 0

/* VFS/FatFS 回归测试：置 1 时 init 进程改为运行 vfs_test()（src/debug/vfs_fatfs_test.c）
 * 再关机，不启动用户程序。覆盖 open/read/write/stat/mkdir/truncate/rename/unlink/rmdir
 * 以及目录读取通路（getdents64 后端：'.'/'..' 合成、d_reclen 对齐、pending 暂存、
 * LFN 长文件名）。串口应看到一串 [PASS]，末尾无 [FAIL]。
 * 必须在 proc_init() 里调用：vfs_lock() 内部的 sem_down() 需要一个有效的当前 pcb，
 * 而且只能有一个执行流在跑——两边同时跑会并发操作同一批测试文件互相干扰。 */
#define DEBUG_VFS_TEST 0

/* U 态 fork/wait4 syscall 验证：置 1 时 init 额外 fork 一个 user/fork_wait.c 编译出的
 * 用户程序（clone 出子进程、子进程 exit(42)、父进程 wait4 收状态），验证
 * sys_clone/sys_wait4 syscall 接线。串口应看到 "child: hi" 和
 * "parent: reaped pid=<N> exitcode=42"。跟 DEBUG_SCHED_TEST 互不冲突，可同时置 1。 */
#define DEBUG_FORK_WAIT_TEST 0

/* dup + execve 验证：置 1 时 fork 一个 user/exectest.c 编译出的用户程序：
 * 它 dup(1) 后经新 fd 写、再 clone 出子进程 execve("/bin/hello.elf")、父进程 wait4 收割。串口应看到 "exectest: hello via dup fd"（dup 生效）、
 * "hi"（子进程 exec 成 hello）、"exectest: child reaped, done"。置 1 时不跑默认用户程序。 */
#define DEBUG_EXEC_TEST 0

/* POSIX 文件 syscall 验证：置 1 时 fork 一个 user/filetest.c 编译出的
 * 用户程序，端到端触发 openat/lseek/readv/writev/fstat/newfstatat/getdents64/mkdirat/
 * unlinkat/renameat/chdir/getcwd/ftruncate/fcntl，并在两个子进程里并发跑文件操作压 VFS 大锁。
 * 这批 syscall 只能由真正的 U 态程序验证——它们都要求真实用户地址空间指针。
 * 串口应看到 "=== filetest done: N pass  0 fail ==="。置 1 时不跑 exectest/默认用户程序。 */
#define DEBUG_FILE_TEST 0

/* 管道 syscall 验证：置 1 时 init fork 一个 user/pipetest.c 编译出的用户程序，
 * 端到端触发 pipe2/read/write/close/dup/fcntl(F_SETFL)/lseek/fstat，覆盖 EOF/EPIPE/
 * EAGAIN/ESPIPE 语义、环形缓冲跨边界、阻塞读写被唤醒、dup 后引用计数、父子管道通信、
 * 多写者原子性、双向 SMP 压测。串口应看到 "=== pipetest done: N pass  0 fail ==="。
 * 置 1 时不跑 exectest/filetest/默认用户程序。 */
#define DEBUG_PIPE_TEST 0

/* TTY 行规范层 + termios/ioctl 验证：置 1 时 init fork 一个 user/ttytest.c 编译出的
 * 用户程序，端到端触发 read(0,...) 的 canonical/raw 行规范（\r->\n、退格、^U、^D
 * EOF、短读续读、缓冲溢出不崩）、O_NONBLOCK、TCGETS/TCSETS/TIOCGWINSZ、非 TTY fd
 * 的 -ENOTTY、lseek -ESPIPE、fstat S_ISCHR、/dev/null、dup/fork 后 fd 0 的共享语义。
 * 输入必须靠管道喂给 QEMU stdin（`printf ... | qemu ...`），交互敲键盘跑不动这套
 * 自动化用例。串口应看到 "=== ttytest done: N pass  0 fail ==="。
 * 置 1 时不跑 exectest/filetest/pipetest/默认用户程序。 */
#define DEBUG_TTY_TEST 0

/* 内存管理 syscall 验证：置 1 时 init fork 一个 user/memtest.c 编译出的用户程序，
 * 端到端触发 brk/mmap/munmap，覆盖堆扩张收缩与越界拒绝、匿名映射懒分配、
 * munmap 四种覆盖情形（含中间打洞导致的 VMA 分裂）、跨多 VMA 解除、地址复用、
 * MAP_PRIVATE 的 fork COW 语义，以及 mmap/munmap 交错 200 轮的压力用例。
 * 串口应看到 "=== memtest done: N pass  0 fail ==="。
 * 置 1 时不跑 exectest/filetest/pipetest/ttytest/默认用户程序。 */
#define DEBUG_MEM_TEST 0

/* 信号验证：置 1 时 init fork 一个 user/sigtest.c 编译出的用户程序，端到端触发
 * rt_sigaction/rt_sigprocmask/rt_sigpending/kill/tkill/rt_sigreturn/setpgid，
 * 覆盖默认动作（终止/忽略）、用户 handler 与自动屏蔽、SA_RESETHAND、
 * 阻塞读被打断的 -EINTR 与 SA_RESTART 重启、SIGPIPE、SIGCHLD、进程组群发、
 * 以及非法访问触发 SIGSEGV（此时内核必须不 panic）。
 * 串口应看到 "=== sigtest done: N pass  0 fail ==="。
 * 置 1 时不跑 exectest/filetest/pipetest/ttytest/memtest/默认用户程序。 */
#define DEBUG_SIGNAL_TEST 0

/* 时间与杂项 syscall 验证：置 1 时先在 ramdisk 塞好 /argvtest，再 fork 一个
 * user/timetest.c 编译出的用户程序，端到端触发 clock_gettime/getres/settime、
 * gettimeofday、nanosleep/clock_nanosleep（含 TIMER_ABSTIME 与被 SIGALRM 打断时
 * 回填 rem）、setitimer/getitimer 的单次与周期定时器、uname/umask/times/
 * sched_yield/set_tid_address/身份四件套，以及 execve 传 argv/envp（由 /argvtest
 * 自校验 argc/argv/envp/auxv 与 sp 对齐，退出码即失败条数）。
 * 串口应看到 "=== argvtest done: ..." 与 "=== timetest done: N pass  0 fail ==="。
 * 置 1 时不跑 exectest/filetest/pipetest/ttytest/memtest/sigtest/默认用户程序。 */
#define DEBUG_TIME_TEST 0

/* ELF 共享页加载验证：置 1 时 fork 一个 user/segtest.c 编译出的用户程序。
 * 它由 user/user_dense.ld 链接，两个 PT_LOAD 段的虚拟地址首尾相接、共用中间那一页
 * ——elf_load 若按"一段一次映射"处理，后一段会给共享页换上一张新的零页，
 * 把前一段落在该页上的 .rodata 整片抹掉。
 * 串口应看到 "=== segtest done: 4 pass  0 fail ==="。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_SEG_TEST 0

/* musl 启动路径验证：置 1 时 fork 一个 user/mhello.c 编译出的用户程序。
 * 它是第一个**不手写 ecall、走 libc** 的程序，用第二套工具链
 * （/root/riscv/toolchain-musl，rv64gc/lp64d）静态链接 musl 编出来。
 * 验的是 crt1.o 的 _start 能否从阶段 8F 铺的初始栈上把 argc/argv/envp/auxv 读出来、
 * __libc_start_main → __init_libc → __init_tls/__init_ssp 一路跑到 main。
 * 串口应看到 "hello from musl"，且**不应出现任何 "syscall: unknown nr="**。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_MUSL_TEST 0

/* libc 级 syscall 覆盖自检：置 1 时 fork 一个 user/msyscheck.c 编译出的用户程序。
 * 与裸 ecall 那批测试的区别是**换了一个客户**：musl 以它自己的方式调内核——
 * stdio 走 writev/readv、opendir/readdir 对 getdents64 的缓冲区与 d_reclen 另有假设、
 * malloc 按尺寸在 brk 与 mmap 之间切换、fork 走 clone(SIGCHLD)、open 会带
 * O_CLOEXEC/O_DIRECTORY 这些我们从没喂过的标志位。
 * 顺带把"内核跑不了浮点"钉成用例：子进程 printf("%f") 必须被 SIGILL 杀掉。
 * 串口应看到 "=== msyscheck done: N pass  0 fail ==="。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_MSYSCHECK_TEST 0

/* rootfs 镜像通路验证：置 1 时 fork 一个 user/mrootfs.c 编译出的用户程序，
 * 验证 tools/build_rootfs.sh 在宿主机造好、由 QEMU -device loader 搬进 rootfs
 * 预留区的 FAT 镜像，内容真的能被内核读到——目录项（含长文件名）、文件内容、
 * ELF 魔数与尾部各验一条。
 * **必须先 `make rootfs`**：镜像不存在时 scripts/run.sh 不会加 -device loader，
 * 内核会 f_mkfs 出一张空盘，这些断言会如实报 FAIL。
 * 串口应看到 "=== mrootfs done: N pass  0 fail ==="。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_MROOTFS_TEST 0

/* wait4 的 pid 选择与 WNOHANG，以及放大后的 fd 表：置 1 时 fork 一个
 * user/waittest.c 编译出的用户程序。三样都是为 ash 补的——它按 pid 跟踪作业、
 * 每次打提示符前做一次非阻塞收割、用 fcntl(F_DUPFD, 10) 把 fd 挪到 10 以上。
 * 串口应看到 "=== waittest done: N pass  0 fail ==="。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_WAIT_TEST 0

/* U 态同步异常不得 panic 内核：置 1 时 fork 一个 user/trapkill.c 编译出的用户程序。
 * 逐条触发 ebreak / 非法指令 / 未对齐访存 / 未对齐取指，断言"只杀掉该子进程、
 * 内核仍在运行"。触发不了的那几条（RV64GC 与 QEMU virt 决定的，见程序内注释）
 * 如实报告为 "no trap"，不伪造异常。
 * 串口应看到 "=== trapkill done: N pass  0 fail ..."。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_TRAP_TEST 0

/* BusyBox 冒烟：置 1 时 fork 一个 /bin/busybox（tools/build_busybox.sh 编、
 * tools/build_rootfs.sh 放进镜像）。它是第一个不由本仓库编写的用户程序。
 * **必须先 `bash tools/build_busybox.sh` 再 `make rootfs`**，否则镜像里没有它。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_BUSYBOX_TEST 0

/* BusyBox 以什么形态启动，仅在 DEBUG_BUSYBOX_TEST 为 1 时有意义：
 *   1 = 交互式 shell（argv 为 {"busybox", "sh"}），**这是默认的交付形态**；
 *   0 = 一串用 && 串起来的冒烟命令，供 `bash scripts/regress.sh bb` 使用。
 * 分成两个开关而不是来回改 proc.c 里那条命令串：改一次忘一次，
 * 而且回归套件的判据就藏在那串命令的收尾标记里，改坏了不会当场报错。 */
#define DEBUG_BUSYBOX_INTERACTIVE 1

/* 浮点上下文验证：置 1 时 fork 一个 user/mfptest.c 编译出的用户程序。
 * 用 musl 工具链编（rv64gc/lp64d），能直接写 double——裸机那套没有 D 扩展。
 * 验的是"切一次进程浮点结果会不会变"：跨 syscall、跨信号投递、跨 16 轮父子
 * ping-pong 切换、fork 继承，以及 setjmp/longjmp 往返（ash 就是这个形状）。
 * 串口应看到 "=== mfptest done: N pass  0 fail ==="。
 * 置 1 时不跑其它测试程序与默认用户程序。 */
#define DEBUG_MFP_TEST 0

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
