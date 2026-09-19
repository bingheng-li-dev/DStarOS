/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/init.c —— 系统的 1 号进程 /sbin/init
 *
 * 它接管的就是原来那个内核线程 init()：**同一个 PCB、同一个 pid=1**，
 * 只是从执行内核代码换成执行这里的用户代码（内核侧走 run_user_program 变身，
 * 不是新建进程）。于是原本在内核态做的两件事一并搬到了用户态：
 *   - 收割孤儿：do_exit() 把孤儿过继给 find_proc_by_pid(1)，那就是本进程；
 *   - 决定"没有子进程之后干什么"：内核态那版是关机，这里是重起一个 shell。
 *
 * 用 musl 而不是手写 ecall：要用的 fork/execl/waitpid/signal 全在 libc 里，
 * 手写这四个 ecall 除了把代码写长没有任何好处。FP 上下文阶段 10 已经补过，
 * musl 程序不再有 SIGILL 风险。
 */

#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL_PATH "/bin/busybox"

/* 不用 printf：stdio 有缓冲，而这里的输出要和 shell 的输出交错着看，
 * 缓冲会让"init 说了什么"和"什么时候说的"对不上。write 是无缓冲的。 */
static void say(const char *msg)
{
    write(1, msg, strlen(msg));
}

int main(void)
{
    /* Linux 里 PID 1 对未注册处理程序的信号天生免疫，那是内核特权；本内核没有这条，
     * 必须显式忽略。**不忽略就是一按 Ctrl+C 整机 panic**：TTY 把 ^C 发给前台进程组，
     * 而前台组的初值正是 init 自己的 pgid，SIGINT 的默认动作会杀掉 pid 1，
     * 随即触发内核 do_exit 里"init 退出即 panic"。
     * BusyBox 关掉了作业控制、ash 不会 tcsetpgrp 改前台组，所以这条路必然走到。 */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    for (;;)
    {
        say("[init] starting shell\n");

        pid_t shell = fork();
        if (shell < 0)
        {
            say("[init] fork failed, retrying\n");
            sleep(1);
            continue;
        }

        if (shell == 0)
        {
            /* argv[0] 写 "busybox" 而不是路径：applet 分发看的就是 argv[0]。
             * 用 execle 显式传环境：内核给 init 的初始 envp 是空的。
             * **不设 PS1**——这份 BusyBox 关掉了 FEATURE_EDITING，连带没有
             * ASH_EXPAND_PRMT，PS1 里的 \w \$ 会被原样打出来，比默认的 "# " 更难看。 */
            static char *const shell_env[] = {
                "HOME=/",
                "PATH=/bin",
                NULL
            };
            execle(SHELL_PATH, "busybox", "sh", (char *)NULL, shell_env);
            say("[init] exec " SHELL_PATH " failed\n");
            _exit(127);
        }

        /* 收到 shell 退出为止。中途收到的是过继过来的孤儿——收掉它们正是 init 的职责，
         * 不能因为"不是我 fork 的"就跳过，否则僵尸会一直堆到 PCB 耗尽。 */
        for (;;)
        {
            pid_t reaped = waitpid(-1, NULL, 0);
            if (reaped == shell)
            {
                break;
            }
            if (reaped < 0)
            {
                /* 到这里只可能是 ECHILD（shell 已经没了）。**绝不能 return**——
                 * init 退出等于系统失去收割者，内核会直接 panic。当作"该重起了"。 */
                break;
            }
        }

        say("[init] shell exited, restarting\n");
    }

    return 0;   /* 不可达 */
}
