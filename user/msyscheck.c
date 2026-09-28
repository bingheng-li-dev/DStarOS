/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/* user/msyscheck.c —— 用 libc 接口复压内核的 syscall
 *
 * 与 user/ 下那批裸 ecall 测试程序的关系：**不是重复，是换一个客户**。
 * 裸 ecall 那批验证全部由我们自己手写的调用发起，参数怎么摆、缓冲区多大、
 * 调用顺序如何，都是按内核实现方便的样子写的。musl 不迁就任何人：
 *   - stdio 走 writev/readv，不是 write/read；
 *   - opendir/readdir 对 getdents64 的缓冲区大小与 d_reclen 有自己的假设；
 *   - malloc 按尺寸在 brk 与 mmap 之间切换；
 *   - fork 走 clone(SIGCHLD)，不是我们习惯的那套参数；
 *   - open 会带上 O_CLOEXEC / O_DIRECTORY 这些我们从没喂过的标志位。
 * 所以这里出现的任何失败**优先怀疑内核**——musl 比我们自己写的夹具可信得多。
 *
 * 约定与其它测试一致：逐条打 PASS/FAIL，末尾一行汇总，退出码 = 失败条数。
 *
 * 浮点格式化（%f/%e/%g）可以用：内核保存 FP 上下文，进 U 态时 sstatus.FS 置成 Initial。
 * 这一点由 test_fp_printf() 显式验证。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>

static int pass_count;
static int fail_count;

static void check(int cond, const char *name)
{
    if (cond)
    {
        pass_count++;
        fputs("  PASS: ", stdout);
    }
    else
    {
        fail_count++;
        fputs("  FAIL: ", stdout);
    }
    puts(name);
}

/* ---------------- stdio：走的是 writev/readv，不是 write/read ---------------- */
static void test_stdio(void)
{
    const char *path = "/msys.txt";
    FILE *f = fopen(path, "w");
    check(f != NULL, "fopen(w)");
    if (!f)
    {
        return;
    }
    size_t n = fwrite("abcdefghij", 1, 10, f);
    check(n == 10, "fwrite 10 bytes");
    check(fclose(f) == 0, "fclose after write");

    f = fopen(path, "r");
    check(f != NULL, "fopen(r)");
    if (!f)
    {
        return;
    }
    char buf[16];
    memset(buf, 0, sizeof(buf));
    n = fread(buf, 1, sizeof(buf), f);
    check(n == 10 && memcmp(buf, "abcdefghij", 10) == 0, "fread reads back what fwrite wrote");

    check(fseek(f, 4, SEEK_SET) == 0, "fseek(SEEK_SET)");
    check(ftell(f) == 4, "ftell after fseek");
    int c = fgetc(f);
    check(c == 'e', "fgetc at offset 4");
    check(fseek(f, 0, SEEK_END) == 0 && ftell(f) == 10, "fseek(SEEK_END)/ftell == size");
    check(fclose(f) == 0, "fclose after read");

    struct stat st;
    check(stat(path, &st) == 0 && st.st_size == 10, "stat st_size == 10");
    check(unlink(path) == 0, "unlink");
    check(stat(path, &st) != 0, "stat after unlink fails");
}

/* ---------------- 目录：opendir/readdir 对 getdents64 的假设与我们的不同 ---------------- */
static void test_dir(void)
{
    const char *dir = "/msysdir";
    (void)rmdir(dir);
    check(mkdir(dir, 0755) == 0, "mkdir");

    /* 在目录里放三个文件，再用 readdir 逐个数回来 */
    static const char *names[3] = {"aa", "bb", "cc"};
    int made = 0;
    for (int i = 0; i < 3; i++)
    {
        char p[64];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd >= 0)
        {
            made++;
            close(fd);
        }
    }
    check(made == 3, "create 3 files in dir");

    /* opendir 内部用的是 open(..., O_RDONLY|O_DIRECTORY|O_CLOEXEC)——
     * 这两个标志位我们的 openat 从没被喂过 */
    DIR *d = opendir(dir);
    check(d != NULL, "opendir (O_DIRECTORY|O_CLOEXEC)");
    if (d)
    {
        int seen = 0, dots = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            {
                dots++;
                continue;
            }
            for (int i = 0; i < 3; i++)
            {
                if (strcmp(e->d_name, names[i]) == 0)
                {
                    seen++;
                }
            }
        }
        check(seen == 3, "readdir returns all 3 entries");
        check(dots >= 1, "readdir returns . / ..");
        check(closedir(d) == 0, "closedir");
    }

    for (int i = 0; i < 3; i++)
    {
        char p[64];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        unlink(p);
    }
    check(rmdir(dir) == 0, "rmdir");
}

/* ---------------- malloc：按尺寸在 brk 与 mmap 之间切换 ---------------- */
static void test_malloc(void)
{
    /* 小块通常走 brk 扩堆 */
    void *small[64];
    int ok = 1;
    for (int i = 0; i < 64; i++)
    {
        small[i] = malloc(128);
        if (!small[i])
        {
            ok = 0;
            break;
        }
        memset(small[i], 0x5a, 128);
    }
    check(ok, "64 x malloc(128)");
    for (int i = 0; i < 64; i++)
    {
        free(small[i]);
    }

    /* 大块走 mmap；写满再校验，确保拿到的确实是可用的整块内存 */
    size_t big_size = 512 * 1024;
    unsigned char *big = malloc(big_size);
    check(big != NULL, "malloc(512K)");
    if (big)
    {
        memset(big, 0xa5, big_size);
        int intact = (big[0] == 0xa5 && big[big_size / 2] == 0xa5 && big[big_size - 1] == 0xa5);
        check(intact, "512K block is fully writable");
        free(big);
    }

    /* realloc 会触发 mmap→brk 之间的搬运 */
    char *p = malloc(32);
    if (p)
    {
        strcpy(p, "hello");
        char *q = realloc(p, 8192);
        check(q != NULL && strcmp(q, "hello") == 0, "realloc keeps contents");
        free(q);
    }
    else
    {
        check(0, "realloc keeps contents");
    }
}

/* ---------------- fork/waitpid：musl 的 fork 走 clone(SIGCHLD) ---------------- */
static void test_fork_wait(void)
{
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0)
    {
        _exit(37);
    }
    int st = 0;
    pid_t got = waitpid(pid, &st, 0);
    check(got == pid, "waitpid returns the child pid");
    check(WIFEXITED(st) && WEXITSTATUS(st) == 37, "child exit status == 37");
}

/* ---------------- pipe + fdopen：把管道包成 FILE*，读写都过 stdio ---------------- */
static void test_pipe_stdio(void)
{
    int fds[2];
    check(pipe(fds) == 0, "pipe");
    if (pipe(fds) != 0)
    {
        return;
    }
    pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        FILE *w = fdopen(fds[1], "w");
        if (w)
        {
            fputs("through-stdio\n", w);
            fclose(w);
        }
        _exit(0);
    }
    close(fds[1]);
    FILE *r = fdopen(fds[0], "r");
    check(r != NULL, "fdopen(pipe read end)");
    char line[64];
    memset(line, 0, sizeof(line));
    char *got = r ? fgets(line, sizeof(line), r) : NULL;
    check(got != NULL && strcmp(line, "through-stdio\n") == 0, "fgets reads the line through the pipe");
    if (r)
    {
        fclose(r);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st), "pipe child exited normally");
}

/* ---------------- 信号：sigaction + raise ---------------- */
static volatile sig_atomic_t got_signal;

static void on_usr1(int sig)
{
    got_signal = sig;
}

static void test_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    check(sigaction(SIGUSR1, &sa, NULL) == 0, "sigaction(SIGUSR1)");
    got_signal = 0;
    check(raise(SIGUSR1) == 0, "raise(SIGUSR1)");
    check(got_signal == SIGUSR1, "handler ran with the right signo");

    /* 屏蔽字往返 */
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    check(sigprocmask(SIG_BLOCK, &set, &old) == 0, "sigprocmask(SIG_BLOCK)");
    sigset_t now;
    check(sigprocmask(SIG_SETMASK, NULL, &now) == 0 && sigismember(&now, SIGUSR2) == 1,
          "SIGUSR2 is blocked after SIG_BLOCK");
    check(sigprocmask(SIG_SETMASK, &old, NULL) == 0, "sigprocmask restore");
}

/* ---------------- 时间 ---------------- */
static void test_time(void)
{
    struct timespec a, b;
    check(clock_gettime(CLOCK_MONOTONIC, &a) == 0, "clock_gettime(MONOTONIC)");

    struct timespec req = {0, 20 * 1000 * 1000}; /* 20ms */
    check(nanosleep(&req, NULL) == 0, "nanosleep(20ms)");

    check(clock_gettime(CLOCK_MONOTONIC, &b) == 0, "clock_gettime again");
    long long delta_ns = (long long)(b.tv_sec - a.tv_sec) * 1000000000LL + (b.tv_nsec - a.tv_nsec);
    check(delta_ns >= 20 * 1000 * 1000, "monotonic clock advanced by at least the sleep");

    check(clock_gettime(CLOCK_REALTIME, &a) == 0, "clock_gettime(REALTIME)");
    check(a.tv_sec > 1700000000L, "realtime looks like a plausible epoch");
}

/* ---------------- uname / cwd ---------------- */
static void test_misc(void)
{
    struct utsname u;
    check(uname(&u) == 0, "uname");
    check(strlen(u.sysname) > 0 && strlen(u.machine) > 0, "uname fields are non-empty");

    char cwd[128];
    check(getcwd(cwd, sizeof(cwd)) != NULL, "getcwd");
    check(cwd[0] == '/', "cwd is absolute");

    check(getpid() > 0, "getpid");
    check(getuid() == 0, "getuid == 0 (single-user)");
}

/* ---------------- 浮点：现在能正常格式化 ---------------- */
static void test_fp_printf(void)
{
    /* 放在子进程里做：万一哪天 FS 又被关回 Off，这里会是 SIGILL 而不是把整个
     * 测试程序带走，父进程还能如实报出来。浮点算得对不对由 mfptest 那套负责。 */
    pid_t pid = fork();
    if (pid == 0)
    {
        volatile double d = 1.5;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", d);
        _exit(strcmp(buf, "1.5") == 0 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "printf(\"%f\") works (kernel now saves FP context)");
}
int main(void)
{
    puts("=== msyscheck: libc-level syscall coverage ===");

    test_stdio();
    test_dir();
    test_malloc();
    test_fork_wait();
    test_pipe_stdio();
    test_signal();
    test_time();
    test_misc();
    test_fp_printf();

    printf("=== msyscheck done: %d pass  %d fail ===\n", pass_count, fail_count);
    return fail_count;
}
