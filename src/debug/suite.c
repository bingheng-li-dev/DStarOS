/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "debug.h"

#if DEBUG_SUITE

#include "suites.h"
#include "proc.h"
#include "console.h"
#include "errorcode.h"
#include "sbi.h"
#include "slab.h"
#include "pmm.h"
#include "vfs.h"

/* 用户态套件跑的程序，都由 tools/build_rootfs.sh 放在 rootfs 镜像的 /bin 下 */
#if DEBUG_SUITE == SUITE_EXEC
#define USER_PROGRAM_PATH "/bin/exectest.elf"
#elif DEBUG_SUITE == SUITE_FILE
#define USER_PROGRAM_PATH "/bin/filetest.elf"
#elif DEBUG_SUITE == SUITE_PIPE
#define USER_PROGRAM_PATH "/bin/pipetest.elf"
#elif DEBUG_SUITE == SUITE_TTY
#define USER_PROGRAM_PATH "/bin/ttytest.elf"
#elif DEBUG_SUITE == SUITE_MEM
#define USER_PROGRAM_PATH "/bin/memtest.elf"
#elif DEBUG_SUITE == SUITE_SIG
#define USER_PROGRAM_PATH "/bin/sigtest.elf"
#elif DEBUG_SUITE == SUITE_TIME
#define USER_PROGRAM_PATH "/bin/timetest.elf"
#elif DEBUG_SUITE == SUITE_SEG
#define USER_PROGRAM_PATH "/bin/segtest.elf"
#elif DEBUG_SUITE == SUITE_MUSL
#define USER_PROGRAM_PATH "/bin/mhello.elf"
#elif DEBUG_SUITE == SUITE_MSYS
#define USER_PROGRAM_PATH "/bin/msyscheck.elf"
#elif DEBUG_SUITE == SUITE_MROOT
#define USER_PROGRAM_PATH "/bin/mrootfs.elf"
#elif DEBUG_SUITE == SUITE_WAIT
#define USER_PROGRAM_PATH "/bin/waittest.elf"
#elif DEBUG_SUITE == SUITE_TRAP
#define USER_PROGRAM_PATH "/bin/trapkill.elf"
#elif DEBUG_SUITE == SUITE_BB || DEBUG_SUITE == SUITE_BB_SH
#define USER_PROGRAM_PATH "/bin/busybox"
#elif DEBUG_SUITE == SUITE_MFP
#define USER_PROGRAM_PATH "/bin/mfptest.elf"
#elif DEBUG_SUITE == SUITE_FORK_WAIT
/* 占位：给收割循环一个跑完就退出的子进程 */
#define USER_PROGRAM_PATH "/bin/hello.elf"
#endif

/* 只有 BusyBox 看 argv，其余程序给一个程序名占住 argv[0] 即可 */
#if DEBUG_SUITE == SUITE_BB_SH
#define USER_PROGRAM_ARGV { "busybox", "sh" }
#elif DEBUG_SUITE == SUITE_BB
/* 用 && 串起来：任何一条失败就短路，收尾标记打不出来，regress.sh 判"suite did not finish"。
 * 每一段对应一类 syscall：echo=write、ls=getdents64/newfstatat、cat=openat/read、
 * mkdir/rmdir=mkdirat/unlinkat、管道=pipe2+clone+wait4、pwd=getcwd、uname、sleep；
 * >/dev/null 走的是 ash 的 savefd()，即 fcntl(F_DUPFD, 10)。 */
#define USER_PROGRAM_ARGV { "busybox", "sh", "-c", "echo bb-echo && ls / >/dev/null && cat /etc/issue >/dev/null && mkdir /tmp/bb && ls /tmp >/dev/null && rmdir /tmp/bb && ls / | cat >/dev/null && pwd >/dev/null && uname >/dev/null && sleep 0 && echo === bbtest done ===" }
#else
#define USER_PROGRAM_ARGV { "init" }
#endif

#ifdef USER_PROGRAM_PATH

static const char *const user_program_argv[] = USER_PROGRAM_ARGV;

static void run_first_user_program(void)
{
    proc_run_user_program(USER_PROGRAM_PATH, user_program_argv,
                          (int)(sizeof(user_program_argv) / sizeof(user_program_argv[0])));
}

#if DEBUG_SUITE == SUITE_FORK_WAIT
static void run_fork_wait_test_program(void)
{
    static const char *const fw_argv[] = { "fork_wait" };
    proc_run_user_program("/bin/fork_wait.elf", fw_argv, 1);
}
#endif

/* fork 出测试程序，然后一直收割到没有子进程为止再关机；
 * scripts/regress.sh 靠 "no more children, shutting down" 之后 QEMU 退出判一套跑完 */
static void run_user_suite(void)
{
#if DEBUG_SUITE == SUITE_FORK_WAIT
    int fw_pid = create_kernel_thread_by_fork((void *)run_fork_wait_test_program, NULL, 0);
    if (fw_pid < 0)
    {
        panic("Failed to fork fork_wait test program thread!\n");
    }
#endif

    int pid = create_kernel_thread_by_fork((void *)run_first_user_program, NULL, 0);
    if (pid < 0)
    {
        panic("Failed to fork user program thread!\n");
    }

    while (1)
    {
        int status;
        int cpid = do_wait(-1, &status, 0);
        if (cpid == ENO24_RESTARTSYS)
        {
            continue;
        }
        if (cpid > 0)
        {
            printf("[init] reaped pid=%d status=%d\n", cpid, (status >> 8) & 0xff);
        }
        else
        {
            printf("[init] no more children, shutting down\n");
#if DEBUG_SUITE == SUITE_MEM || DEBUG_SUITE == SUITE_FILE
            /* 压力用例跑完之后核对各 cache 的 nr_inuse 是否回到基线（vma_cache 尤其）*/
            slab_dump_stats();
            vfs_dcache_stats();
            printf("[init] pmm_free_list.fnsize=%d\n", pmm_free_list.fnsize);
#endif
            sbi_shutdown();
        }
    }
}

#endif /* USER_PROGRAM_PATH */

/**
 * @brief 按 DEBUG_SUITE 跑一套回归，跑完关机
 * @note 由内核态 init（pid 1）调用：内核态套件以 init 为驱动直接跑；
 *   用户态套件 fork 出测试程序，init 留在内核态做收割循环。
 */
void debug_suite_run(void)
{
#if DEBUG_SUITE == SUITE_SCHED
    run_sched_tests();
    printf("[init] scheduler tests done, shutting down\n");
    sbi_shutdown();
#elif DEBUG_SUITE == SUITE_SLAB
    run_slab_tests();
    sbi_shutdown();
#elif DEBUG_SUITE == SUITE_DCACHE
    run_dcache_tests();
    sbi_shutdown();
#elif DEBUG_SUITE == SUITE_VFS
    vfs_test();
    sbi_shutdown();
#elif defined(USER_PROGRAM_PATH)
    run_user_suite();
#else
#error "DEBUG_SUITE 不是 debug.h 里定义的 SUITE_* 之一"
#endif
}

#endif /* DEBUG_SUITE */
