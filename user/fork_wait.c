/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/fork_wait.c —— U 态 fork/exit/wait4/COW 测试程序
 * 验证 sys_clone/sys_wait4 的 syscall 接线（由真正的用户进程调用），以及 vmm.c 里的写时复制。
 * 父进程 clone 出子进程；子进程改写共享的全局变量后 write+exit(42)；父进程 wait4 收子进程
 * 退出码，检查自己看到的全局变量是否仍是原值（验证父子地址空间已经拆分、不是还在共享），
 * 再自己写一次同一变量（验证子进程退出后引用计数应已降到 1，走的是原地补写权限而不是
 * 再分配一次的优化路径——这一步的行为差异只能从内核 DEBUG_VMM_page_fault_handler 的日志里看，
 * 用户态观察不到，但两次写操作本身的正确性在这里就能验证）。不引入 libc，写法与 hello.c 一致。
 */

#define __NR_write 64
#define __NR_exit  93
#define __NR_clone 220
#define __NR_wait4 260

static inline long syscall4(long nr, long a0, long a1, long a2, long a3)
{
    register long r_a7 asm("a7") = nr;
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a3 asm("a3") = a3;
    asm volatile("ecall"
                 : "+r"(r_a0)
                 : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a7)
                 : "memory");
    return r_a0;
}

static long sys_write(int fd, const char *buf, unsigned long len)
{
    return syscall4(__NR_write, fd, (long)buf, (long)len, 0);
}

static void sys_exit(int code)
{
    syscall4(__NR_exit, code, 0, 0, 0);
    for (;;)
    {
        /* exit 系统调用不应返回；万一返回则原地死循环兜底 */
    }
}

static long sys_clone(void)
{
    return syscall4(__NR_clone, 0, 0, 0, 0);
}

static long sys_wait4(long pid, int *wstatus, long options)
{
    return syscall4(__NR_wait4, pid, (long)wstatus, options, 0);
}

/* 把 [0, 999] 内的无符号整数转成十进制串写到 fd，不依赖 libc 的 itoa/printf */
static void write_uint(unsigned int v)
{
    char digits[4];
    int n = 0;
    if (v == 0)
    {
        digits[n++] = '0';
    }
    while (v > 0)
    {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    }

    char buf[4];
    int i;
    for (i = 0; i < n; i++)
    {
        buf[i] = digits[n - 1 - i];
    }
    sys_write(1, buf, (unsigned long)n);
}

/* fork 前父子共享的全局变量：位于 .data 段，fork 时会被 vmm_mm_copy 标成只读共享页，
 * 谁先写谁触发 COW 拆分。用 volatile 防止编译器把读写优化掉。 */
static volatile int shared_val = 111;

static int main(void)
{
    long pid = sys_clone();

    if (pid == 0)
    {
        /* 子进程：clone 的返回值约定，参照 do_fork 里 x10_a0 置 0 的实现。
         * 这次写会触发 COW 拆分（此刻该帧被父子共享，reference==2），
         * 子进程应该拿到一份独立的物理页，不影响父进程看到的值。 */
        shared_val = 222;
        const char msg[] = "child: hi, wrote 222\n";
        sys_write(1, msg, sizeof(msg) - 1);
        sys_exit(42);
    }
    else if (pid > 0)
    {
        /* 父进程：阻塞等待任意子进程退出，取回收割到的 pid 与退出码 */
        int status = 0;
        long wpid = sys_wait4(-1, &status, 0);

        const char msg1[] = "parent: reaped pid=";
        sys_write(1, msg1, sizeof(msg1) - 1);
        write_uint((unsigned int)wpid);

        const char msg2[] = " exitcode=";
        sys_write(1, msg2, sizeof(msg2) - 1);
        write_uint((unsigned int)((status >> 8) & 0xff));

        const char nl[] = "\n";
        sys_write(1, nl, 1);

        /* COW 正确性核心断言：子进程已经退出并写过 222，父进程这里必须仍看到 111，
         * 否则说明父子曾经共享的是同一份物理内存，COW 没有真正拆分开。 */
        const char msg3[] = "parent: shared_val after child exit=";
        sys_write(1, msg3, sizeof(msg3) - 1);
        write_uint((unsigned int)shared_val);
        sys_write(1, "\n", 1);

        /* 子进程退出时 do_exit->vmm_unmap_vma 已经把它那份独立页的 reference 减到 0
         * 释放掉了，父进程这份原始页此刻 reference 应该已经回落到 1（不再共享）。
         * 这次写触发 COW 拆分逻辑里 reference==1 的原地补写权限分支，不会再分配新页。 */
        shared_val = 333;
        const char msg4[] = "parent: shared_val after own write=";
        sys_write(1, msg4, sizeof(msg4) - 1);
        write_uint((unsigned int)shared_val);
        sys_write(1, "\n", 1);
    }
    else
    {
        const char msg[] = "fork failed\n";
        sys_write(1, msg, sizeof(msg) - 1);
    }

    return 0;
}

/* 程序入口：见 user/hello.c 对 _start 约定的说明，此处完全一致。 */
void _start(void)
{
    int code = main();
    sys_exit(code);
}
