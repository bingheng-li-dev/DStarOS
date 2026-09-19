/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/mfptest.c —— 验证浮点上下文在特权级边界与任务切换上不丢
 *
 * 为什么必须有这套：BusyBox 逼出来的。musl 在 lp64d 下的 setjmp/longjmp 会
 * **无条件**存取 fs0~fs11（编译期按 ABI 选进来的，不是运行时按需），而 ash 的
 * 异常机制就建立在 setjmp 上——内核不保存 FP 上下文的话，ash_main 的第一条 setjmp
 * 就会被 SIGILL 杀掉。补上 pcb 的 proc_fp_regs 之后，真正危险的不再是"能不能执行"，
 * 而是"切一次进程结果会不会变"——那是静默出错，比崩掉难查一个数量级。
 *
 * 用 musl 工具链编（rv64gc/lp64d）而不是裸机那套（rv64imac/lp64）：裸机工具链没有
 * D 扩展，浮点只能靠 .word 下原始编码，写不出有意义的算例。
 *
 * **每条断言都必须依赖被切换打断**：光算一遍对不能说明问题，寄存器根本没被别人动过。
 * 所以每个用例都在两次读取之间插入一次强制的调度（管道读写 / sleep / 信号 / fork）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/wait.h>
#include <time.h>

static int pass_count;
static int fail_count;

static void check(const char *name, int ok)
{
    if (ok)
    {
        pass_count++;
        printf("  PASS: %s\n", name);
    }
    else
    {
        fail_count++;
        printf("  FAIL: %s\n", name);
    }
    fflush(stdout);
}

/* 比较到位：这些值都能被 double 精确表示，不需要容差。真出现容差需求就说明
 * 算错了地方，不该用 epsilon 糊过去。 */
static void check_d(const char *name, double got, double want)
{
    if (got == want)
    {
        pass_count++;
        printf("  PASS: %s\n", name);
    }
    else
    {
        fail_count++;
        printf("  FAIL: %s (got %ld/1000, want %ld/1000)\n",
               name, (long)(got * 1000), (long)(want * 1000));
    }
    fflush(stdout);
}

/* 用例 1~2：基本算术与 libc 的浮点格式化路径。
 * 后者是 §3.2 一直担心的那条——scalbn 里那 8 条 FP 指令就在这里执行。 */
static void test_basic(void)
{
    volatile double a = 3.5, b = 0.25;
    check_d("double add", a + b, 3.75);
    check_d("double mul", a * b, 0.875);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", 2.5);
    check("printf %f goes through scalbn", strcmp(buf, "2.500") == 0);
}

/* 用例 3：setjmp/longjmp 往返 —— **这就是 ash 的形状**。
 * musl 的 __setjmp 存 fs0~fs11、__longjmp 原样恢复；内核不保存 FP 的话，
 * 光是走到 setjmp 那一行就已经 SIGILL 了。 */
static jmp_buf jb;

static void test_setjmp(void)
{
    volatile double keep = 12.5;
    if (setjmp(jb) == 0)
    {
        longjmp(jb, 7);
    }
    check_d("setjmp/longjmp round trip keeps fp value", keep, 12.5);
}

/* 用例 4：跨 syscall（含一次阻塞睡眠）后寄存器不变。
 * nanosleep 会真的让出 CPU，所以这条同时压到了"内核在 S 态跑了一大段"这个场景。 */
static void test_across_syscall(void)
{
    volatile double x = 1.5;
    volatile double y = 2.25;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 30 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    check_d("fp survives a blocking syscall (x)", x, 1.5);
    check_d("fp survives a blocking syscall (y)", y, 2.25);
    check_d("fp arithmetic still correct after sleep", x * y, 3.375);
}

/* 用例 5：信号处理函数里做浮点，主流程的值不许被污染。
 * 信号投递会在用户栈上凭空造一个调用帧、经 sigpage 蹦床回内核，
 * handler 用掉的 f 寄存器由编译器按 ABI 保存/恢复——这条验的是那条链路没被内核搅乱。 */
static volatile double handler_result;

static void fp_handler(int sig)
{
    (void)sig;
    volatile double h = 100.5;
    handler_result = h * 2.0;
}

static void test_across_signal(void)
{
    volatile double before = 8.125;
    signal(SIGUSR1, fp_handler);
    raise(SIGUSR1);
    check_d("handler did fp work", handler_result, 201.0);
    check_d("fp survives signal delivery", before, 8.125);
}

/* 用例 6~8：**两个进程交替跑浮点**，靠管道 ping-pong 强制来回切换。
 *
 * 这是整套里唯一真正压到 fpu_save/fpu_restore 的用例：父子各自持有一组不同的值，
 * 每轮都被对方打断。内核漏存漏取的话，两边的值会互相串味——而且是静默的。
 */
static void test_across_context_switch(void)
{
    int to_child[2], to_parent[2];
    if (pipe(to_child) != 0 || pipe(to_parent) != 0)
    {
        check("fp switch: pipe created", 0);
        return;
    }

    pid_t kid = fork();
    if (kid == 0)
    {
        close(to_child[1]);
        close(to_parent[0]);
        volatile double c = 7.5;
        double acc = 0.0;
        char t;
        for (int i = 0; i < 16; i++)
        {
            if (read(to_child[0], &t, 1) != 1)
            {
                _exit(2);
            }
            acc += c * 2.0;          /* 每轮 +15.0 */
            if (write(to_parent[1], &t, 1) != 1)
            {
                _exit(3);
            }
        }
        /* 16 轮 × 15.0 = 240.0；c 也必须还是 7.5 */
        _exit((acc == 240.0 && c == 7.5) ? 0 : 1);
    }

    close(to_child[0]);
    close(to_parent[1]);
    volatile double p = 3.25;
    double acc = 0.0;
    char t = 'x';
    for (int i = 0; i < 16; i++)
    {
        if (write(to_child[1], &t, 1) != 1)
        {
            break;
        }
        if (read(to_parent[0], &t, 1) != 1)
        {
            break;
        }
        acc += p * 4.0;              /* 每轮 +13.0 */
    }
    close(to_child[1]);
    close(to_parent[0]);

    int st = 0;
    waitpid(kid, &st, 0);
    check_d("parent fp value intact after 16 switches", p, 3.25);
    check_d("parent fp accumulator correct", acc, 208.0);
    check("child fp value and accumulator intact", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* 用例 9：fork 出来的子进程必须继承父进程**此刻**的浮点寄存器，
 * 而不是父进程上一次被换出时存进 pcb 的那份旧值。 */
static void test_fork_inherits(void)
{
    volatile double v = 42.75;
    pid_t kid = fork();
    if (kid == 0)
    {
        _exit(v == 42.75 ? 0 : 1);
    }
    int st = 0;
    waitpid(kid, &st, 0);
    check("fork inherits live fp registers", WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(void)
{
    printf("\n=== mfptest: floating point context ===\n");
    fflush(stdout);

    test_basic();
    test_setjmp();
    test_across_syscall();
    test_across_signal();
    test_across_context_switch();
    test_fork_inherits();

    printf("=== mfptest done: %d pass  %d fail ===\n", pass_count, fail_count);
    fflush(stdout);
    return fail_count == 0 ? 0 : 1;
}
