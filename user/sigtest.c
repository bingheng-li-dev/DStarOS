/* user/sigtest.c —— 验证信号（rt_sigaction/rt_sigprocmask/kill/rt_sigreturn 等）
 *
 * 信号机制的**唯一真实验收手段**：投递发生在"内核带着本进程返回 U 态"那一刻，
 * 要改写 trap 帧、在用户栈上凭空造出一个函数调用帧、再靠 sigpage 蹦床跳回内核，
 * 这整条链路在内核态自测里没法模拟——只有真正的 U 态进程才走得通。
 *
 * 写法与 pipetest.c / memtest.c 一致：不引入 libc，syscall 全部内联 ecall。
 * 用户态没有 nanosleep/sched_yield，"让对方先阻塞住"靠 busy_delay() 忙等制造窗口。
 *
 * **信号用错的典型表现是挂死或跑飞，而不是干净地报错**：凡是要等对端反应的用例，
 * 子进程都带一个有限的兜底循环（超时就 exit(9)），这样内核逻辑错误时这个程序会
 * 报 FAIL 而不是让 QEMU 停在那里不动。
 */

#define __NR_pipe2   59
#define __NR_close   57
#define __NR_read    63
#define __NR_write   64
#define __NR_exit    93
#define __NR_kill   129
#define __NR_tkill  130
#define __NR_rt_sigaction   134
#define __NR_rt_sigprocmask 135
#define __NR_rt_sigpending  136
#define __NR_setpgid 154
#define __NR_getpgid 155
#define __NR_getpid 172
#define __NR_clone  220
#define __NR_wait4  260

/* 信号号（Linux asm-generic）*/
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGILL  4
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
#define SIGSTOP 19

#define SIG_DFL 0UL
#define SIG_IGN 1UL

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_SIGINFO   0x00000004UL
#define SA_RESTART   0x10000000UL
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL

/* Linux errno（syscall 失败时返回其负值）*/
#define ESRCH  3
#define EINTR  4
#define EINVAL 22
#define EAGAIN 11
#define EPIPE  32

#define O_NONBLOCK 0x0800

/* 内核 ABI 的 struct sigaction：24 字节，sa_mask 在**最后**，没有 sa_restorer
 * （riscv64 未定义 SA_RESTORER）。顺序写错会把 handler 装成垃圾指针。 */
struct k_sigaction
{
    unsigned long sa_handler;
    unsigned long sa_flags;
    unsigned long sa_mask;
};

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
static long sys_pipe2(int *fds, int flags)
{
    return syscall4(__NR_pipe2, (long)fds, flags, 0, 0);
}
static long sys_getpid(void) { return syscall4(__NR_getpid, 0, 0, 0, 0); }
static long sys_kill(long pid, int sig) { return syscall4(__NR_kill, pid, sig, 0, 0); }
static long sys_tkill(long tid, int sig) { return syscall4(__NR_tkill, tid, sig, 0, 0); }
static long sys_setpgid(long pid, long pgid)
{
    return syscall4(__NR_setpgid, pid, pgid, 0, 0);
}
static long sys_getpgid(long pid) { return syscall4(__NR_getpgid, pid, 0, 0, 0); }
static long sys_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *oact)
{
    return syscall4(__NR_rt_sigaction, sig, (long)act, (long)oact, 8);
}
static long sys_sigprocmask(int how, const unsigned long *set, unsigned long *oset)
{
    return syscall4(__NR_rt_sigprocmask, how, (long)set, (long)oset, 8);
}
static long sys_sigpending(unsigned long *set)
{
    return syscall4(__NR_rt_sigpending, (long)set, 8, 0, 0);
}
static long sys_clone(void)              { return syscall4(__NR_clone, 0, 0, 0, 0); }
static long sys_wait4(long pid, int *ws) { return syscall4(__NR_wait4, pid, (long)ws, 0, 0); }
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

static void busy_delay(long n)
{
    volatile long i;
    for (i = 0; i < n; i++)
    {
    }
}

/* wait status 的两种编码：低 7 位是信号号，为 0 才表示正常退出 */
static int wifsignaled(int st)  { return (st & 0x7f) != 0; }
static int wtermsig(int st)     { return st & 0x7f; }
static int wexitstatus(int st)  { return (st >> 8) & 0xff; }

static void install(int sig, void (*fn)(int), unsigned long flags)
{
    struct k_sigaction sa;
    sa.sa_handler = (unsigned long)fn;
    sa.sa_flags = flags;
    sa.sa_mask = 0;
    sys_sigaction(sig, &sa, 0);
}

static void install_raw(int sig, unsigned long handler, unsigned long flags)
{
    struct k_sigaction sa;
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sa.sa_mask = 0;
    sys_sigaction(sig, &sa, 0);
}

/* ============================================================
 * handler 与它们用到的全局量（会被 handler 改，必须 volatile）
 * ============================================================ */
static volatile int usr1_count;
static volatile int chld_count;
static volatile int in_handler;
static volatile int reentered;
static volatile int nested_raised;

static void h_usr1(int sig)
{
    (void)sig;
    usr1_count++;
}

static void h_chld(int sig)
{
    (void)sig;
    chld_count++;
}

static void h_nested(int sig)
{
    (void)sig;
    if (in_handler)
    {
        reentered = 1;
    }
    in_handler = 1;
    /* 只补发一次，否则每次 handler 都补发一次就是无限循环 */
    if (!nested_raised)
    {
        nested_raised = 1;
        sys_kill(sys_getpid(), SIGUSR1);
        busy_delay(200000); /* 给"如果没屏蔽就会立刻重入"留出足够窗口 */
    }
    in_handler = 0;
    usr1_count++;
}

/* ============================================================
 * 用例 1：rt_sigaction 能把刚装进去的动作原样取回来
 * ============================================================ */
static void test_sigaction_roundtrip(void)
{
    struct k_sigaction sa;
    struct k_sigaction old;

    sa.sa_handler = (unsigned long)h_usr1;
    sa.sa_flags = SA_RESTART;
    sa.sa_mask = 0;
    check_eq("sigaction: install SIGUSR1", sys_sigaction(SIGUSR1, &sa, 0), 0);

    old.sa_handler = 0;
    old.sa_flags = 0;
    check_eq("sigaction: read back handler", sys_sigaction(SIGUSR1, 0, &old), 0);
    check_eq("sigaction: handler matches",
             (long)(old.sa_handler == (unsigned long)h_usr1), 1);
    check_eq("sigaction: flags match", (long)old.sa_flags, (long)SA_RESTART);

    /* sigsetsize 必须是 8 */
    check_eq("sigaction: bad sigsetsize rejected",
             syscall4(__NR_rt_sigaction, SIGUSR1, 0, 0, 4), -EINVAL);
}

/* ============================================================
 * 用例 2：SIGKILL / SIGSTOP 不可捕获，SA_SIGINFO 明确拒绝
 * ============================================================ */
static void test_uncatchable(void)
{
    struct k_sigaction sa;
    sa.sa_handler = (unsigned long)h_usr1;
    sa.sa_flags = 0;
    sa.sa_mask = 0;

    check_eq("sigaction: SIGKILL rejected", sys_sigaction(SIGKILL, &sa, 0), -EINVAL);
    check_eq("sigaction: SIGSTOP rejected", sys_sigaction(SIGSTOP, &sa, 0), -EINVAL);
    check_eq("sigaction: sig 0 rejected", sys_sigaction(0, &sa, 0), -EINVAL);
    check_eq("sigaction: sig 64 rejected", sys_sigaction(64, &sa, 0), -EINVAL);

    sa.sa_flags = SA_SIGINFO;
    check_eq("sigaction: SA_SIGINFO rejected", sys_sigaction(SIGUSR2, &sa, 0), -EINVAL);
}

/* ============================================================
 * 用例 3/4：屏蔽字 + 挂起集
 * ============================================================ */
static void test_mask_and_pending(void)
{
    unsigned long set = 1UL << (SIGUSR1 - 1);
    unsigned long old = 0;
    unsigned long pend = 0;

    install(SIGUSR1, h_usr1, 0);
    usr1_count = 0;

    check_eq("sigprocmask: block SIGUSR1", sys_sigprocmask(SIG_BLOCK, &set, &old), 0);
    sys_kill(sys_getpid(), SIGUSR1);
    check_eq("blocked signal does not run handler", usr1_count, 0);

    check_eq("sigpending: syscall ok", sys_sigpending(&pend), 0);
    check_eq("sigpending: SIGUSR1 bit set", (long)((pend & set) != 0), 1);

    check_eq("sigprocmask: unblock SIGUSR1", sys_sigprocmask(SIG_UNBLOCK, &set, 0), 0);
    /* 解除屏蔽之后要等下一次进内核才投递，随便做一次 syscall 即可 */
    sys_getpid();
    check_eq("unblocked signal runs handler", usr1_count, 1);

    /* SIGKILL 屏蔽不掉：置进去之后读回来应当是 0 */
    unsigned long kill_set = 1UL << (SIGKILL - 1);
    sys_sigprocmask(SIG_SETMASK, &kill_set, 0);
    sys_sigprocmask(SIG_BLOCK, 0, &old);
    check_eq("sigprocmask: SIGKILL cannot be blocked", (long)(old & kill_set), 0);

    unsigned long empty = 0;
    sys_sigprocmask(SIG_SETMASK, &empty, 0);
}

/* ============================================================
 * 用例 5：handler 正常返回，主流程从被打断的地方继续
 * ============================================================ */
static void test_handler_returns(void)
{
    install(SIGUSR1, h_usr1, 0);
    usr1_count = 0;

    volatile long marker = 0x5a5a;
    sys_kill(sys_getpid(), SIGUSR1);
    check_eq("handler ran once", usr1_count, 1);
    check_eq("main flow resumed with intact locals", marker, 0x5a5a);

    /* tkill 与 kill 等价（无线程组） */
    sys_tkill(sys_getpid(), SIGUSR1);
    check_eq("tkill delivers too", usr1_count, 2);
}

/* ============================================================
 * 用例 6：handler 执行期间同号信号被自动屏蔽（不重入）
 * ============================================================ */
static void test_no_reentry(void)
{
    install(SIGUSR1, h_nested, 0);
    usr1_count = 0;
    in_handler = 0;
    reentered = 0;
    nested_raised = 0;

    sys_kill(sys_getpid(), SIGUSR1);

    check_eq("handler not re-entered while running", reentered, 0);
    check_eq("second signal delivered after return", usr1_count, 2);

    install(SIGUSR1, h_usr1, 0);
}

/* ============================================================
 * 用例 7：SA_RESETHAND —— 第二次触发走默认动作（终止）
 * ============================================================ */
static void test_resethand(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        install(SIGUSR1, h_usr1, SA_RESETHAND);
        usr1_count = 0;
        sys_kill(sys_getpid(), SIGUSR1);
        if (usr1_count != 1)
        {
            sys_exit(9); /* 第一次就没跑 handler */
        }
        sys_kill(sys_getpid(), SIGUSR1); /* 此时动作已回到 SIG_DFL，应当被杀掉 */
        sys_exit(8);                     /* 走到这里说明没被杀 */
    }

    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("SA_RESETHAND: second delivery kills", (long)wtermsig(st), SIGUSR1);
}

/* ============================================================
 * 用例 8：默认动作终止 —— 路线图阶段 7 的正式验收项
 * ============================================================ */
static void test_default_terminate(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        sys_kill(sys_getpid(), SIGINT);
        sys_exit(9); /* 走到这里说明默认动作没生效 */
    }

    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("default action: killed by SIGINT", (long)wifsignaled(st), 1);
    check_eq("default action: WTERMSIG == SIGINT", (long)wtermsig(st), SIGINT);
}

/* ============================================================
 * 用例 9：SIG_IGN —— 被忽略的信号既不杀进程也不跑 handler
 * ============================================================ */
static void test_sig_ign(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        install_raw(SIGTERM, SIG_IGN, 0);
        sys_kill(sys_getpid(), SIGTERM);
        sys_kill(sys_getpid(), SIGTERM);
        sys_exit(7); /* 应当活着走到这里 */
    }

    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("SIG_IGN: process survived", (long)wifsignaled(st), 0);
    check_eq("SIG_IGN: exit code intact", (long)wexitstatus(st), 7);
}

/* ============================================================
 * 用例 10/12：阻塞读被信号打断 → -EINTR；再读一次仍然正常
 *   （第二条是在验证被打断的任务确实把自己从等待队列上摘了下来，
 *     漏摘的话同一个 proc_wait_linker 会同时挂在两条链上）
 * ============================================================ */
static void test_eintr_and_requeue(void)
{
    int fd[2];  /* 数据管道，父 → 子 */
    int rep[2]; /* 报告管道，子 → 父；两端都是 O_NONBLOCK，父轮询它判断子进程的进度 */
    sys_pipe2(fd, 0);
    sys_pipe2(rep, O_NONBLOCK);

    install(SIGUSR1, h_usr1, 0); /* 不带 SA_RESTART */

    long cpid = sys_clone();
    if (cpid == 0)
    {
        char cb[8] = {0};
        long r1 = sys_read(fd[0], cb, 1);
        char code = (r1 == -EINTR) ? 'E' : 'X';
        sys_write(rep[1], &code, 1); /* 先报告，父进程据此停止发信号 */
        if (code != 'E')
        {
            sys_exit(1);
        }
        long r2 = sys_read(fd[0], cb, 1);
        if (r2 != 1 || cb[0] != 'B')
        {
            sys_exit(2);
        }
        sys_exit(0);
    }

    /* **不能只发一次信号**：子进程被调度到、走进 pipe_read 需要多久没有上界，
     * 信号如果赶在它睡下去之前到达，就会被当场消费掉，那次 read 反而不会被打断。
     * 改成"发一次、看一眼报告管道"的循环：只要子进程还没报告就继续发，
     * 一旦报告到手立刻停手——停手之后子进程的第二次 read 才不会被误伤。 */
    char code = 0;
    int got = 0;
    for (int i = 0; i < 40 && !got; i++)
    {
        sys_kill(cpid, SIGUSR1);
        busy_delay(1000000);
        if (sys_read(rep[0], &code, 1) == 1)
        {
            got = 1;
        }
    }
    check_eq("blocking read: child reported back", (long)got, 1);
    check_eq("blocking read interrupted -> EINTR", (long)code, 'E');

    sys_write(fd[1], "B", 1);

    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("re-read after EINTR works (wait queue not corrupted)",
             (long)wexitstatus(st), 0);

    sys_close(fd[0]);
    sys_close(fd[1]);
    sys_close(rep[0]);
    sys_close(rep[1]);
}

/* ============================================================
 * 用例 10b：**只发一次**信号也必须打中已经睡在管道上的进程
 *
 * 上一个用例的父进程是循环重发的，那样恰好会把"置 INTERRUPTIBLE 与真正睡下去
 * 之间丢唤醒"这个窗口盖住——重发一次就补上了。这里只发一次，专盯那个窗口。
 *
 * 子进程先经 rdy 管道报告"马上进 read"，父进程收到后再忙等一段时间才发信号，
 * 确保信号落在"确实已经睡着"的时刻。兜底：发完信号等足够久之后往数据管道写一个
 * 字节把子进程捞回来——这样即便内核真丢了唤醒，用例也是干净地报 FAIL，
 * 而不是让 wait4 挂死、整个回归停在那里不动。
 * ============================================================ */
static void test_single_kill_wakes_blocked(void)
{
    int fd[2];
    int rdy[2];
    sys_pipe2(fd, 0);
    sys_pipe2(rdy, 0);

    install(SIGUSR1, h_usr1, 0);

    long cpid = sys_clone();
    if (cpid == 0)
    {
        char cb[8] = {0};
        sys_write(rdy[1], "R", 1);
        long r = sys_read(fd[0], cb, 1);
        /* r == 1 说明是被兜底的那个字节捞回来的，即信号没能把它叫醒 */
        sys_exit(r == -EINTR ? 0 : 1);
    }

    char ready = 0;
    sys_read(rdy[0], &ready, 1); /* 阻塞等子进程报告 */
    busy_delay(4000000);         /* 再等它真的睡进 pipe_read */
    sys_kill(cpid, SIGUSR1);     /* **只发这一次** */

    busy_delay(20000000);        /* 正常内核早该醒了 */
    sys_write(fd[1], "X", 1);    /* 兜底捞人 */

    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("single kill wakes a process already blocked in read",
             (long)wexitstatus(st), 0);

    sys_close(fd[0]);
    sys_close(fd[1]);
    sys_close(rdy[0]);
    sys_close(rdy[1]);
}

/* ============================================================
 * 用例 11：SA_RESTART —— 被打断的 read 自动重启，用户完全看不到这次打断
 * ============================================================ */
static void test_sa_restart(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    install(SIGUSR1, h_usr1, SA_RESTART);
    /* **必须在 clone 之前清零**：放在子进程里清的话，信号若赶在那条赋值之前到达，
     * handler 的自增会被随后的清零抹掉，用例就会假失败 */
    usr1_count = 0;

    long cpid = sys_clone();
    if (cpid == 0)
    {
        char cb[8] = {0};
        long r = sys_read(fd[0], cb, 1);
        if (r != 1 || cb[0] != 'R')
        {
            sys_exit(1); /* 带 SA_RESTART 就不该看到 -EINTR */
        }
        if (usr1_count < 1)
        {
            sys_exit(2); /* handler 必须真的跑过，否则这个用例什么也没测到 */
        }
        sys_exit(0);
    }

    /* 连发几次：只要有一次落在阻塞窗口里，重启逻辑就被真正走到了；
     * 落在窗口外的那几次也无害（SA_RESTART 下多跑几次 handler 不改变结果）。
     * "确实是在阻塞中被打断"这条由上一个用例的握手式判据确定性地覆盖。 */
    for (int i = 0; i < 3; i++)
    {
        busy_delay(2000000);
        sys_kill(cpid, SIGUSR1);
    }
    busy_delay(2000000);
    sys_write(fd[1], "R", 1);

    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("SA_RESTART: read restarted instead of returning EINTR",
             (long)wexitstatus(st), 0);

    sys_close(fd[0]);
    sys_close(fd[1]);
    install(SIGUSR1, h_usr1, 0);
}

/* ============================================================
 * 用例 13：kill 的存在性检查（sig == 0）
 * ============================================================ */
static void test_kill_existence(void)
{
    check_eq("kill(self, 0) == 0", sys_kill(sys_getpid(), 0), 0);
    check_eq("kill(9999, 0) == -ESRCH", sys_kill(9999, 0), -ESRCH);
    check_eq("kill(self, 99) == -EINVAL", sys_kill(sys_getpid(), 99), -EINVAL);
    check_eq("kill(-1, SIGTERM) == -EINVAL", sys_kill(-1, SIGTERM), -EINVAL);
}

/* ============================================================
 * 用例 14：SIGPIPE —— 默认杀死；SIG_IGN 时退化成 -EPIPE 返回值
 * ============================================================ */
static void test_sigpipe(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        int cfd[2];
        sys_pipe2(cfd, 0);
        sys_close(cfd[0]);
        sys_write(cfd[1], "x", 1);
        sys_exit(9); /* 应当已被 SIGPIPE 杀掉 */
    }
    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("SIGPIPE: default action kills writer", (long)wtermsig(st), SIGPIPE);

    cpid = sys_clone();
    if (cpid == 0)
    {
        install_raw(SIGPIPE, SIG_IGN, 0);
        int cfd[2];
        sys_pipe2(cfd, 0);
        sys_close(cfd[0]);
        long w = sys_write(cfd[1], "x", 1);
        sys_exit(w == -EPIPE ? 0 : 1);
    }
    st = 0;
    sys_wait4(cpid, &st);
    check_eq("SIGPIPE ignored: write returns -EPIPE", (long)wexitstatus(st), 0);
}

/* ============================================================
 * 用例 15：SIGCHLD —— 子进程退出时父进程收到，且 wait4 被 SA_RESTART 重启
 * ============================================================ */
static void test_sigchld(void)
{
    install(SIGCHLD, h_chld, SA_RESTART);
    chld_count = 0;

    long cpid = sys_clone();
    if (cpid == 0)
    {
        busy_delay(2000000); /* 让父进程先睡进 do_wait，SIGCHLD 才有唤醒可打断 */
        sys_exit(3);
    }

    int st = 0;
    long r = sys_wait4(cpid, &st);
    check_eq("SIGCHLD: wait4 still returned the child", r, cpid);
    check_eq("SIGCHLD: exit code intact across restart", (long)wexitstatus(st), 3);
    check_eq("SIGCHLD: handler ran", (long)(chld_count >= 1), 1);

    /* 复位成 SIG_DFL（默认忽略），免得后面的用例被额外的 SIGCHLD 干扰 */
    install_raw(SIGCHLD, SIG_DFL, 0);
}

/* ============================================================
 * 用例 16：进程组 —— kill(-pgid, sig) 一次打中整组
 * ============================================================ */
static void test_process_group(void)
{
    long c1 = sys_clone();
    if (c1 == 0)
    {
        /* 兜底循环：万一组信号没打中，这里也会自己退出，不会把测试挂死 */
        for (long i = 0; i < 200; i++)
        {
            busy_delay(200000);
        }
        sys_exit(9);
    }
    long newpg = c1;
    check_eq("setpgid: make child1 a group leader", sys_setpgid(c1, newpg), 0);
    check_eq("getpgid: child1 in new group", sys_getpgid(c1), newpg);

    long c2 = sys_clone();
    if (c2 == 0)
    {
        for (long i = 0; i < 200; i++)
        {
            busy_delay(200000);
        }
        sys_exit(9);
    }
    sys_setpgid(c2, newpg);

    busy_delay(1000000);
    check_eq("kill(-pgid, SIGTERM) ok", sys_kill(-newpg, SIGTERM), 0);

    int st1 = 0;
    int st2 = 0;
    sys_wait4(c1, &st1);
    sys_wait4(c2, &st2);
    check_eq("group kill: child1 terminated by SIGTERM", (long)wtermsig(st1), SIGTERM);
    check_eq("group kill: child2 terminated by SIGTERM", (long)wtermsig(st2), SIGTERM);

    /* 父进程自己不在那个组里，应当毫发无损 */
    check_eq("group kill: parent untouched", sys_kill(sys_getpid(), 0), 0);
}

/* ============================================================
 * 用例 17：非法访问 → SIGSEGV 杀掉该进程，**内核不 panic**
 * ============================================================ */
static void test_sigsegv(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        volatile char *bad = (volatile char *)1;
        *bad = 'x';
        sys_exit(9);
    }
    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("bad store: killed by SIGSEGV", (long)wtermsig(st), SIGSEGV);
}

/* ============================================================
 * 用例 18：浮点指令**正常执行**，不再被 SIGILL 杀
 *
 * 这条用例原本断言的是反面（FS 关死 + SIGILL 探针）。BusyBox 进来之后内核补上了
 * 真正的 FP 上下文（pcb 里的 proc_fp_regs/proc_fcsr + 切换时存取），进 U 态的
 * trapframe 把 sstatus.FS 置成 Initial，浮点于是可以正常用了。
 *
 * **判据翻面了，但防线没撤**：如果哪天有人把 FS 又关回 Off、或者把 fpu_save/
 * fpu_restore 摘掉一半，这条会立刻变红。浮点算得对不对由 mfptest 那套负责
 * （它用 musl 工具链编，能直接写 double），这里只管"能不能执行"。
 *
 * 用 .word 下原始编码而不是写 fadd.d：本程序按 -march=rv64imac 编译（无 D 扩展），
 * 汇编器不认这条助记符。0x02000053 = fadd.d f0,f0,f0（已用 objdump 核对）。
 * ============================================================ */
static void test_fp_no_longer_traps(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        asm volatile(".word 0x02000053");
        sys_exit(9);
    }
    int st = 0;
    sys_wait4(cpid, &st);
    check_eq("fp instruction: not killed by any signal", (long)wtermsig(st), 0);
    check_eq("fp instruction: child ran to completion", (long)wexitstatus(st), 9);
}

void _start(void)
{
    puts_fd(1, "\n=== sigtest: signal syscalls ===\n");

    test_sigaction_roundtrip();
    test_uncatchable();
    test_mask_and_pending();
    test_handler_returns();
    test_no_reentry();
    test_resethand();
    test_default_terminate();
    test_sig_ign();
    test_eintr_and_requeue();
    test_single_kill_wakes_blocked();
    test_sa_restart();
    test_kill_existence();
    test_sigpipe();
    test_sigchld();
    test_process_group();
    test_sigsegv();
    test_fp_no_longer_traps();

    puts_fd(1, "=== sigtest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");

    sys_exit(fail_count == 0 ? 0 : 1);
}
