/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/mhello.c —— 第一个用 musl 静态链接的用户程序
 *
 * 与 user/ 下其它程序的根本不同：它**不手写 ecall**，而是走 libc。
 * 目的只有一个——把"musl 的启动路径能不能在本内核上跑通"这一件事单独验干净：
 *   crt1.o 的 _start 从初始栈读 argc/argv/envp/auxv（内核 setup_user_stack 铺的那套）
 *     → __libc_start_main → __init_libc 遍历 auxv
 *     → __init_tls / __init_ssp（读 AT_RANDOM 做栈保护 canary）
 *     → 设置 tp 寄存器 → main
 * 这条路上缺任何一个 syscall，内核都会打 "syscall: unknown nr=<号>"。
 *
 * 刻意保持极简：不 fork、不开文件、不碰信号，那些留给 msyscheck。这里多做一件事，失败时就多一个
 * 要排除的变量。
 *
 * printf 之后不显式 fflush：musl 在 exit 时会冲刷 stdio，正好顺带验证
 * "退出路径上的 writev" 也是通的。
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    printf("hello from musl\n");
    printf("argc=%d argv[0]=%s\n", argc, (argc > 0 && argv[0]) ? argv[0] : "(null)");

    /* malloc 走 brk 或 mmap；这里只要不返回 NULL 即可，真正的覆盖在 msyscheck。 */
    void *p = malloc(64);
    printf("malloc(64)=%s\n", p ? "ok" : "NULL");
    free(p);

    /* 收尾标记：regress.sh 靠它判定"这一轮真的跑完了"。必须是单次 write 能打出的
     * 一整串——另一个 hart 的内核 printf 只会插在两次 write 之间，不会切开一串。 */
    printf("=== mhello done: ok ===\n");
    return 0;
}
