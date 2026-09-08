/* user/waittest.c —— 验证 wait4 的 pid 选择与 WNOHANG，以及放大后的 fd 表
 *
 * 这两样都是**为 ash 补的**，而不是为了补齐 POSIX：
 *   - ash 按 pid 跟踪作业，收错一个就是 $? 错、或者一个已死的作业永远等不到；
 *   - ash 每次打提示符之前会做一次非阻塞收割，WNOHANG 被忽略的话整个 shell
 *     会睡死在提示符之前；
 *   - ash 的 savefd() 是 fcntl(fd, F_DUPFD, 10)，刻意把 fd 挪到 10 以上，
 *     而 NOFILE 原先只有 16。
 *
 * 写法与 sigtest.c / pipetest.c 一致：不引入 libc，syscall 全部内联 ecall。
 *
 * **"子进程还活着"这个前提不能靠忙等制造**——那是概率而不是保证。这里让子进程
 * 阻塞在一个空管道的读端上：只要父进程没写那一个字节，子进程就一定还活着，
 * WNOHANG 必须返回 0。
 */

#define __NR_openat  56
#define __NR_close   57
#define __NR_pipe2   59
#define __NR_read    63
#define __NR_write   64
#define __NR_fcntl   25
#define __NR_exit    93
#define __NR_clone  220
#define __NR_wait4  260

#define AT_FDCWD   (-100)
#define O_RDONLY   0

#define F_DUPFD    0

#define WNOHANG    1

/* Linux errno（syscall 失败时返回其负值）*/
#define ECHILD 10
#define ENOSYS 38

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
static long sys_read(int fd, void *buf, unsigned long len)
{
    return syscall4(__NR_read, fd, (long)buf, (long)len, 0);
}
static long sys_close(int fd) { return syscall4(__NR_close, fd, 0, 0, 0); }
static long sys_openat(const char *path, int flags)
{
    return syscall4(__NR_openat, AT_FDCWD, (long)path, flags, 0);
}
static long sys_pipe2(int *fds, int flags)
{
    return syscall4(__NR_pipe2, (long)fds, flags, 0, 0);
}
static long sys_fcntl(int fd, int cmd, long arg)
{
    return syscall4(__NR_fcntl, fd, cmd, arg, 0);
}
static long sys_clone(void)  { return syscall4(__NR_clone, 0, 0, 0, 0); }
static long sys_wait4(long pid, int *ws, int options)
{
    return syscall4(__NR_wait4, pid, (long)ws, options, 0);
}
static void sys_exit(int code)
{
    syscall4(__NR_exit, code, 0, 0, 0);
    for (;;)
    {
        /* exit 不应返回；万一返回则原地死循环兜底 */
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

static void puts_fd(int fd, const char *s)
{
    sys_write(fd, s, ustrlen(s));
}

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

static void check_true(const char *name, long cond)
{
    check_eq(name, cond ? 1 : 0, 1);
}

static int wexitstatus(int st) { return (st >> 8) & 0xff; }

/* 用例 1~2：三个子进程，父进程**按创建的中间顺序**逐个 waitpid。
 *
 * 这是"认 pid"最直接的判据：改动之前 do_wait 会收割**任意**一个 ZOMBIE，
 * 于是 waitpid(c2) 极可能返回 c1——返回的 pid 与退出码同时都对不上。 */
static void test_wait_by_pid(void)
{
    long codes[3] = { 11, 12, 13 };
    long kids[3];

    for (int i = 0; i < 3; i++)
    {
        long p = sys_clone();
        if (p == 0)
        {
            sys_exit((int)codes[i]);
        }
        kids[i] = p;
    }

    /* 收割顺序 1,0,2 —— 刻意错开创建顺序 */
    const int order[3] = { 1, 0, 2 };
    for (int k = 0; k < 3; k++)
    {
        int i = order[k];
        int st = 0;
        long got = sys_wait4(kids[i], &st, 0);
        check_eq("waitpid(pid) returns that very pid", got, kids[i]);
        check_eq("waitpid(pid) returns that child's code", wexitstatus(st), codes[i]);
    }
}

/* 用例 3~4：WNOHANG。
 *
 * 子进程阻塞在空管道的读端上——只要父进程不写那个字节，它就一定还活着，
 * 所以"WNOHANG 返回 0"是**确定的**而不是概率性的。忙等制造不出这个保证。 */
static void test_wnohang(void)
{
    int fds[2] = { -1, -1 };
    if (sys_pipe2(fds, 0) < 0)
    {
        check_true("wnohang: pipe2 ok", 0);
        return;
    }

    long kid = sys_clone();
    if (kid == 0)
    {
        char c = 0;
        sys_close(fds[1]);
        sys_read(fds[0], &c, 1);   /* 阻塞直到父进程写 */
        sys_exit(7);
    }
    sys_close(fds[0]);

    int st = 0;
    check_eq("WNOHANG with a live child returns 0", sys_wait4(-1, &st, WNOHANG), 0);

    char go = 'g';
    sys_write(fds[1], &go, 1);
    sys_close(fds[1]);

    /* 子进程被放行后总会退出，但"什么时候"取决于调度，所以这里是有界重试。
     * 上限给得很松：它验的是"WNOHANG 最终能收到"，不是延迟。 */
    long got = 0;
    for (int i = 0; i < 100000 && got == 0; i++)
    {
        got = sys_wait4(-1, &st, WNOHANG);
    }
    check_eq("WNOHANG eventually reaps the child", got, kid);
    check_eq("WNOHANG reaped child's exit code", wexitstatus(st), 7);
}

/* 用例 5~7：三种"等不到"的返回值。
 *
 * 前两条是 POSIX 的 ECHILD；第三条（按进程组等待）刻意返回 ENOSYS 而不是退化成
 * "等任意"——ash 关掉 job control 之后不该走到这里，**走到了就说明配置没关净**。 */
static void test_wait_errors(void)
{
    int st = 0;
    check_eq("waitpid(-1) with no children returns -ECHILD",
             sys_wait4(-1, &st, 0), -ECHILD);
    /* pid 1 是 init，永远不是本进程的子进程 */
    check_eq("waitpid(not-my-child) returns -ECHILD",
             sys_wait4(1, &st, 0), -ECHILD);
    check_eq("waitpid(-1, WNOHANG) with no children returns -ECHILD",
             sys_wait4(-1, &st, WNOHANG), -ECHILD);
    check_eq("waitpid(0) (process group) returns -ENOSYS",
             sys_wait4(0, &st, 0), -ENOSYS);
    check_eq("waitpid(-2) (process group) returns -ENOSYS",
             sys_wait4(-2, &st, 0), -ENOSYS);
}

/* 用例 8~11：fd 表放大后 F_DUPFD 能真的落到高位。
 *
 * 这条补的是"实现写对了但从没被走过"——proc_fd_alloc_from(from) 的语义从阶段 3
 * 起就是对的，但此前没有任何用例传过大于 3 的 from。 */
static void test_dupfd_high(void)
{
    long fd = sys_openat("/etc/issue", O_RDONLY);
    if (fd < 0)
    {
        check_true("dupfd: open /etc/issue ok", 0);
        return;
    }

    long d10 = sys_fcntl((int)fd, F_DUPFD, 10);
    check_true("F_DUPFD(10) lands at fd >= 10", d10 >= 10);

    /* 40 这个数字本身就是断言：它只有在 NOFILE > 40 时才可能成立，
     * 也就是说这一条会在 NOFILE 被改回 16 时立刻变红。 */
    long d40 = sys_fcntl((int)fd, F_DUPFD, 40);
    check_eq("F_DUPFD(40) returns exactly 40", d40, 40);

    /* 高位 fd 必须是真的能用的槽，不只是一个编号 */
    char buf[8] = { 0 };
    check_true("read through the high fd works", sys_read((int)d40, buf, sizeof(buf)) > 0);

    check_eq("close high fd", sys_close((int)d40), 0);
    check_eq("close dup fd", sys_close((int)d10), 0);
    check_eq("close orig fd", sys_close((int)fd), 0);
}

void _start(void)
{
    puts_fd(1, "\n=== waittest: wait4 pid/WNOHANG and fd table ===\n");

    test_wait_by_pid();
    test_wnohang();
    test_wait_errors();
    test_dupfd_high();

    puts_fd(1, "=== waittest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");

    sys_exit(fail_count == 0 ? 0 : 1);
}
