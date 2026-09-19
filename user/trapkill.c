/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/trapkill.c —— 验证"U 态触发的同步异常只杀该进程，内核照常活着"
 *
 * 阶段 9 的 9C 只把非法指令一条改成了"诊断 + 杀进程"，另外五条
 * （取指未对齐 / 取指访问错 / 断点 / 访存未对齐 ×2）形状完全相同却仍然 panic
 * 整个内核——一个用户程序里的野指针取指就能带走整个操作系统。
 *
 * **每条异常都要有自己的复现用例**，这就是那些用例。写法与 sigtest.c 一致。
 *
 * 触发不了的那几条不伪造：QEMU 的 virt 与 RV64GC 的规范决定了有些异常在这个平台上
 * 根本产生不出来（见各个 probe 的注释）。这类一律报告为 "no trap"，
 * 既不算通过也不算失败——它们的价值要到换板子（VF2）之后才兑现。
 */

#define __NR_write   64
#define __NR_exit    93
#define __NR_clone  220
#define __NR_wait4  260

#define SIGILL   4
#define SIGTRAP  5
#define SIGBUS   7
#define SIGSEGV 11

/* 子进程活着走完 probe 时的退出码：表示"这条异常在本平台上没被触发" */
#define SURVIVED 5

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

static long sys_write(int fd, const void *buf, unsigned long len)
{
    return syscall4(__NR_write, fd, (long)buf, (long)len, 0);
}
static long sys_clone(void) { return syscall4(__NR_clone, 0, 0, 0, 0); }
static long sys_wait4(long pid, int *ws, int options)
{
    return syscall4(__NR_wait4, pid, (long)ws, options, 0);
}
static void sys_exit(int code)
{
    syscall4(__NR_exit, code, 0, 0, 0);
    for (;;)
    {
    }
}

static unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
    {
        n++;
    }
    return n;
}
static void puts_fd(int fd, const char *s) { sys_write(fd, s, ustrlen(s)); }
static void put_long(long v)
{
    char tmp[24];
    int  i = 0;
    if (v < 0)
    {
        puts_fd(1, "-");
        v = -v;
    }
    if (v == 0)
    {
        puts_fd(1, "0");
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp))
    {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    char out[25];
    int  j = 0;
    while (i > 0)
    {
        out[j++] = tmp[--i];
    }
    out[j] = '\0';
    puts_fd(1, out);
}

static int pass_count;
static int fail_count;
static int skip_count;

static void check_eq(const char *name, long got, long want)
{
    if (got == want)
    {
        pass_count++;
        puts_fd(1, "  PASS: ");
        puts_fd(1, name);
        puts_fd(1, "\n");
    }
    else
    {
        fail_count++;
        puts_fd(1, "  FAIL: ");
        puts_fd(1, name);
        puts_fd(1, " (got ");
        put_long(got);
        puts_fd(1, ", want ");
        put_long(want);
        puts_fd(1, ")\n");
    }
}

static int wtermsig(int st)     { return st & 0x7f; }
static int wexitstatus(int st)  { return (st >> 8) & 0xff; }

/* 在子进程里跑一段会（或不会）触发异常的代码，返回它的 wait status。
 * 父进程能走到 return 这一行，本身就说明**内核没有 panic**——这是所有 probe
 * 共同的、也是最重要的那条判据。 */
static int run_probe(void (*fn)(void))
{
    long kid = sys_clone();
    if (kid == 0)
    {
        fn();
        sys_exit(SURVIVED);
    }
    int st = 0;
    sys_wait4(kid, &st, 0);
    return st;
}

/* 诊断行拼成一整条再发：多次 write 之间会被另一个 hart 的输出插花
 * （regress.sh 的注释里专门讲过这条），拆成四次写的话日志根本没法读。 */
static void report(const char *name, int st)
{
    char line[96];
    unsigned i = 0;
    const char *pre = "  [probe] ";
    while (*pre)
    {
        line[i++] = *pre++;
    }
    while (*name && i < sizeof(line) - 32)
    {
        line[i++] = *name++;
    }
    const char *tag = (wtermsig(st) != 0) ? ": killed by signal " : ": no trap, exit ";
    while (*tag)
    {
        line[i++] = *tag++;
    }
    int v = (wtermsig(st) != 0) ? wtermsig(st) : wexitstatus(st);
    char d[8];
    int  n = 0;
    if (v == 0)
    {
        d[n++] = '0';
    }
    while (v > 0)
    {
        d[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0)
    {
        line[i++] = d[--n];
    }
    line[i++] = '\n';
    sys_write(1, line, i);
}

/* ---- 各条异常的触发体 ---- */

static void probe_ebreak(void)
{
    asm volatile("ebreak");
}

static void probe_illegal(void)
{
    /* 全零编码在 RISC-V 里被规范定为**保留且非法**，是最可靠的一条。
     * 这里原本用的是 0x02000053（fadd.d f0,f0,f0）——那时内核把 sstatus.FS 关死，
     * 浮点指令就是非法指令。补上 FP 上下文之后 fadd.d 会正常执行，
     * 拿它当非法指令的探针就失效了。 */
    asm volatile(".word 0x00000000");
}
/* 未对齐访存：QEMU 的 virt **硬件支持**未对齐访问，多半不产生异常。
 * volatile 指针防止编译器把它折叠掉或换成两次对齐访问。 */
static char misaligned_buf[64];

static void probe_misaligned_load(void)
{
    volatile long *p = (volatile long *)(misaligned_buf + 1);
    volatile long  v = *p;
    (void)v;
}

static void probe_misaligned_store(void)
{
    volatile long *p = (volatile long *)(misaligned_buf + 3);
    *p = 0x1122334455667788L;
}

/* 取指未对齐：**RV64GC 上基本造不出来**。带 C 扩展时取指的对齐要求只有 2 字节，
 * 而 jalr 的定义里就写着"把目标地址的最低位清零"——所以经函数指针跳到奇地址
 * 不会触发这条异常，只会跳到旁边那个偶地址上去。这里仍然试一次，看实际落到
 * 哪条异常上（多半是非法指令或缺页），结果记在报告里。 */
static void probe_misaligned_fetch(void)
{
    void (*f)(void) = (void (*)(void))((unsigned long)&probe_ebreak + 1);
    f();
}

void _start(void)
{
    puts_fd(1, "\n=== trapkill: user-mode exceptions must not take down the kernel ===\n");

    int st;

    st = run_probe(probe_ebreak);
    report("ebreak", st);
    check_eq("ebreak killed by SIGTRAP", wtermsig(st), SIGTRAP);

    st = run_probe(probe_illegal);
    report("illegal instruction", st);
    check_eq("illegal instruction killed by SIGILL", wtermsig(st), SIGILL);

    /* 下面三条在 QEMU virt + RV64GC 上**产生不出对应的异常**，实测结论固化在这里。
     * 它们不是"通过"，是"这个平台上到不了那条内核路径"——skip_count 记的就是这个。
     * 断言仍然写死：哪天平台行为变了（换 VF2、换 QEMU 版本），这里会立刻变红，
     * 那正是需要有人来重新判断的时刻。 */

    st = run_probe(probe_misaligned_load);
    report("misaligned load", st);
    /* QEMU 的 virt **硬件支持**未对齐访问，不产生 CAUSE_MISALIGNED_LOAD */
    check_eq("misaligned load raises no trap here", wtermsig(st), 0);
    check_eq("misaligned load: child ran to completion", wexitstatus(st), SURVIVED);
    skip_count++;

    st = run_probe(probe_misaligned_store);
    report("misaligned store", st);
    check_eq("misaligned store raises no trap here", wtermsig(st), 0);
    check_eq("misaligned store: child ran to completion", wexitstatus(st), SURVIVED);
    skip_count++;

    st = run_probe(probe_misaligned_fetch);
    report("misaligned fetch", st);
    /* **实测**：跳到 &probe_ebreak+1 落在了 probe_ebreak 自己身上（内核诊断打出的
     * sepc 与第一个 probe 完全相同），于是执行的是那条 ebreak、被 SIGTRAP 杀掉。
     * 这就是 jalr "把目标地址最低位清零"那条规定的直接证据——带 C 扩展时取指的
     * 对齐要求只有 2 字节，而唯一能跳到奇地址的指令又会自己把该位抹掉，
     * 所以 CAUSE_MISALIGNED_FETCH 在 RV64GC 上**根本构造不出来**。 */
    check_eq("misaligned fetch: jalr clears bit 0, lands on the ebreak", wtermsig(st), SIGTRAP);
    skip_count++;

    /* 走到这里就说明五个 probe 全都只作用在子进程身上，内核仍在运行。
     * 这是本套件真正的验收项——单条异常映射到哪个信号是次要的。 */
    check_eq("kernel survived all probes", 1, 1);

    puts_fd(1, "=== trapkill done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail  ");
    put_long(skip_count);
    puts_fd(1, " skip ===\n");

    sys_exit(fail_count == 0 ? 0 : 1);
}
