/* user/pipetest.c —— 验证 Phase 4 的管道（pipe2/pipe_read/pipe_write/pipe_release）
 *
 * pipe2/pipe_read/pipe_write 这批 syscall 的**唯一真实验收手段**：内核态自测
 * （src/debug/pipe_test.c）手工构造 pipe_t 绕开了 pipe2 本身与 fd 表整合的部分，
 * 只有真正的 U 态程序、真实用户地址空间指针、真实 fork 出的父子进程，才能端到端
 * 验证 pipe2 → fd 表 → 读写 → 关闭 这条完整链路，以及父子共享 fd 表之后管道的
 * 引用计数语义。不引入 libc，写法与 filetest.c 一致。
 *
 * 用户态没有 sched_yield/nanosleep 之类的 syscall（Phase 8 才有），验证"确实阻塞
 * 过又被唤醒"的用例（阻塞读/阻塞写）靠 busy_delay() 忙等制造时间窗口，再用
 * "子进程最终确实拿到了数据/确实写完了"这个功能性结果间接证明——阻塞/唤醒机制
 * 本身的正确性已经在 pipe_test.c 里用轮询 wq_read/wq_write.task_list 精确验证过，
 * 这里不重复验证机制细节，只验证黑盒行为。
 *
 * 管道用错的表现是挂死而不是崩溃：凡是可能阻塞的用例，对端都不依赖"发起阻塞的
 * 一方自己超时退出"，而是由不阻塞的一方在合理时间内完成动作后 wait4 收割，
 * 如果内核逻辑错误导致真正的死锁，这个程序会挂在 wait4 上，QEMU 侧表现为
 * 不再输出——出现这种情况直接说明 Step 3/4 的阻塞/唤醒逻辑有回归。
 */

#define __NR_dup     23
#define __NR_fcntl   25
#define __NR_pipe2   59
#define __NR_close   57
#define __NR_lseek   62
#define __NR_read    63
#define __NR_write   64
#define __NR_fstat   80
#define __NR_exit    93
#define __NR_clone  220
#define __NR_wait4  260

#define O_NONBLOCK 0x0800

#define SEEK_SET 0
#define SEEK_END 2

#define F_SETFL 4

/* Linux errno（syscall 失败时返回其负值）*/
#define EAGAIN 11
#define ESPIPE 29
#define EPIPE  32

#define S_IFMT  0xF000
#define S_IFIFO 0x1000
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)

/* Linux riscv64 asm-generic struct stat，128 字节，字段偏移必须与内核一致
 * （与 filetest.c 完全相同的布局，这里只用到 st_mode）*/
struct linux_stat
{
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_rdev;
    unsigned long __pad1;
    long          st_size;
    int           st_blksize;
    int           __pad2;
    long          st_blocks;
    long          st_atime;
    long          st_atime_nsec;
    long          st_mtime;
    long          st_mtime_nsec;
    long          st_ctime;
    long          st_ctime_nsec;
    unsigned int  __unused_end[2];
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
static long sys_dup(int oldfd) { return syscall4(__NR_dup, oldfd, 0, 0, 0); }
static long sys_fcntl(int fd, int cmd, long arg)
{
    return syscall4(__NR_fcntl, fd, cmd, arg, 0);
}
static long sys_lseek(int fd, long off, int whence)
{
    return syscall4(__NR_lseek, fd, off, whence, 0);
}
static long sys_fstat(int fd, struct linux_stat *st)
{
    return syscall4(__NR_fstat, fd, (long)st, 0, 0);
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

static int ustrcmp(const char *a, const char *b)
{
    unsigned long i = 0;
    while (a[i] && a[i] == b[i])
    {
        i++;
    }
    return (int)((unsigned char)a[i] - (unsigned char)b[i]);
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

static int pass_count = 0;
static int fail_count = 0;

static void check(const char *name, int cond)
{
    puts_fd(1, cond ? "  [PASS] " : "  [FAIL] ");
    puts_fd(1, name);
    puts_fd(1, "\n");
    if (cond)
    {
        pass_count++;
    }
    else
    {
        fail_count++;
    }
}

static void check_eq(const char *name, long got, long want)
{
    int ok = (got == want);
    check(name, ok);
    if (!ok)
    {
        puts_fd(1, "         want=");
        put_long(want);
        puts_fd(1, " got=");
        put_long(got);
        puts_fd(1, "\n");
    }
}

/* 用户态没有 sched_yield/nanosleep，只能用忙等制造时间窗口，提高对端先跑到
 * 阻塞点的概率——不追求 100% 确定性，测试的判据始终是最终功能结果是否正确，
 * 阻塞/唤醒机制本身已经在 pipe_test.c 里精确验证过。 */
static void busy_delay(long n)
{
    volatile long i;
    for (i = 0; i < n; i++)
    {
    }
}

/* ============================================================
 * 用例 1：pipe2(fd, 0) 基本形状
 * ============================================================ */
static void test_pipe2_basic(void)
{
    int fd[2];
    long r = sys_pipe2(fd, 0);
    check_eq("pipe2(0) returns 0", r, 0);
    check("pipe2: fd[0] != fd[1]", fd[0] != fd[1]);
    check("pipe2: fd[0] >= 0", fd[0] >= 0);
    check("pipe2: fd[1] >= 0", fd[1] >= 0);
    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 2：写 "hello" 再读，内容与长度一致
 * ============================================================ */
static void test_write_read_roundtrip(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    check_eq("write hello (5 bytes)", sys_write(fd[1], "hello", 5), 5);
    char buf[8] = {0};
    check_eq("read hello (5 bytes)", sys_read(fd[0], buf, 5), 5);
    check("read content == hello", ustrcmp(buf, "hello") == 0);

    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 3：关写端后读 —— 返回 0（EOF），既不阻塞也不报错
 * ============================================================ */
static void test_eof_after_write_close(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    sys_close(fd[1]);
    char b;
    check_eq("read after write end closed -> EOF(0)", sys_read(fd[0], &b, 1), 0);

    sys_close(fd[0]);
}

/* ============================================================
 * 用例 4：关读端后写 —— 返回 -EPIPE
 * ============================================================ */
static void test_epipe_after_read_close(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    sys_close(fd[0]);
    check_eq("write after read end closed -> -EPIPE", sys_write(fd[1], "x", 1), -EPIPE);

    sys_close(fd[1]);
}

/* ============================================================
 * 用例 5：环形回绕 —— 写 3000/读 3000 反复 5 轮，累计 15000 > 4096(PIPE_SIZE)，
 * 必然多次跨越缓冲区末端；每轮填不同字符，防止"读到旧一轮残留但没检测出来"
 * ============================================================ */
static void test_wraparound(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    static char wbuf[3000];
    static char rbuf[3000];
    int ok = 1;
    for (int round = 0; round < 5 && ok; round++)
    {
        char fill = (char)('A' + round);
        for (int i = 0; i < 3000; i++)
        {
            wbuf[i] = fill;
        }
        if (sys_write(fd[1], wbuf, 3000) != 3000)
        {
            ok = 0;
            break;
        }
        for (int i = 0; i < 3000; i++)
        {
            rbuf[i] = 0;
        }
        if (sys_read(fd[0], rbuf, 3000) != 3000)
        {
            ok = 0;
            break;
        }
        for (int i = 0; i < 3000; i++)
        {
            if (rbuf[i] != fill)
            {
                ok = 0;
                break;
            }
        }
    }
    check("wraparound: 5 rounds of 3000B write/read all correct", ok);

    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 6：阻塞读 —— fork 出子进程先 read（此时管道为空，必然阻塞），
 * 父进程忙等一段时间后再写，子进程应能正确收到数据
 * ============================================================ */
static void test_blocking_read(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    long cpid = sys_clone();
    if (cpid == 0)
    {
        char cb[8] = {0};
        long r = sys_read(fd[0], cb, 5);
        int ok = (r == 5) && (ustrcmp(cb, "block") == 0);
        sys_exit(ok ? 0 : 1);
    }

    busy_delay(2000000);
    sys_write(fd[1], "block", 5);

    int cst = 0;
    sys_wait4(cpid, &cst);
    check_eq("blocking read: child got data after delayed parent write",
             (cst >> 8) & 0xff, 0);

    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 7：阻塞写 —— 先写满 4096 字节，子进程再写 1 字节（必然阻塞），
 * 父进程忙等后读走数据腾出空间，子进程应能完成这次写
 * ============================================================ */
static void test_blocking_write(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    static char big[4096];
    for (int i = 0; i < 4096; i++)
    {
        big[i] = 'B';
    }
    check_eq("fill pipe with 4096 bytes", sys_write(fd[1], big, 4096), 4096);

    long cpid = sys_clone();
    if (cpid == 0)
    {
        long w = sys_write(fd[1], "X", 1);
        sys_exit(w == 1 ? 0 : 1);
    }

    busy_delay(2000000);
    static char drain[4096];
    check_eq("drain 4096 bytes to make room", sys_read(fd[0], drain, 4096), 4096);

    int cst = 0;
    sys_wait4(cpid, &cst);
    check_eq("blocking write: child completed pending write after drain",
             (cst >> 8) & 0xff, 0);

    char one;
    sys_read(fd[0], &one, 1); /* 收尾：读走子进程写的那 1 字节 */
    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 8：O_NONBLOCK —— 空管道读 / 满管道写均返回 -EAGAIN，不阻塞
 * ============================================================ */
static void test_nonblock(void)
{
    int fd[2];
    sys_pipe2(fd, O_NONBLOCK);

    char b;
    check_eq("O_NONBLOCK empty read -> -EAGAIN", sys_read(fd[0], &b, 1), -EAGAIN);

    static char big[4096];
    for (int i = 0; i < 4096; i++)
    {
        big[i] = 'C';
    }
    check_eq("fill pipe (O_NONBLOCK)", sys_write(fd[1], big, 4096), 4096);
    check_eq("O_NONBLOCK full write -> -EAGAIN", sys_write(fd[1], "Y", 1), -EAGAIN);

    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 9：lseek(pipe_fd, ...) —— 均返回 -ESPIPE，且不崩
 * ============================================================ */
static void test_lseek_espipe(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    check_eq("lseek SEEK_SET on pipe read end -> -ESPIPE",
             sys_lseek(fd[0], 0, SEEK_SET), -ESPIPE);
    check_eq("lseek SEEK_END on pipe write end -> -ESPIPE",
             sys_lseek(fd[1], 0, SEEK_END), -ESPIPE);

    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 10：fstat(pipe_fd) —— S_ISFIFO(st_mode) 为真
 * ============================================================ */
static void test_fstat_fifo(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    struct linux_stat st;
    check_eq("fstat pipe read end ok", sys_fstat(fd[0], &st), 0);
    check("fstat pipe: S_ISFIFO(st_mode)", S_ISFIFO(st.st_mode));

    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 11：父子传一行字符串（路线图阶段 4 的正式验收项）
 * ============================================================ */
static void test_parent_child_message(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    long cpid = sys_clone();
    if (cpid == 0)
    {
        sys_close(fd[1]); /* 子进程规范用法：关掉自己不需要的写端 */
        char msg[32] = {0};
        long r = sys_read(fd[0], msg, sizeof(msg) - 1);
        int ok = (r > 0) && (ustrcmp(msg, "hello from parent") == 0);
        sys_exit(ok ? 0 : 1);
    }

    sys_close(fd[0]); /* 父进程规范用法：关掉自己不需要的读端 */
    sys_write(fd[1], "hello from parent", 18);
    sys_close(fd[1]);

    int cst = 0;
    sys_wait4(cpid, &cst);
    check_eq("parent-child pipe: child received exact message", (cst >> 8) & 0xff, 0);
}

/* ============================================================
 * 用例 12：dup 写端后关掉其中一份 —— 读端不应看到 EOF
 * （验证 readers/writers 没有被 dup/close 一份就误减）
 * ============================================================ */
static void test_dup_write_end(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    long dupw = sys_dup(fd[1]);
    check("dup write end ok", dupw >= 0);
    sys_close(fd[1]); /* 关掉原写端 fd，dup 出来的那份还活着 */

    sys_fcntl(fd[0], F_SETFL, O_NONBLOCK);
    char b;
    check_eq("dup keeps writer alive: nonblocking empty read -> -EAGAIN (not EOF)",
             sys_read(fd[0], &b, 1), -EAGAIN);

    check_eq("write via duped fd", sys_write((int)dupw, "Z", 1), 1);
    check_eq("read gets byte written via duped fd", sys_read(fd[0], &b, 1), 1);
    check("content correct (Z)", b == 'Z');

    sys_close((int)dupw);
    sys_close(fd[0]);
}

/* ============================================================
 * 用例 13：父忘了关自己的写端 —— 固化经典陷阱：只要任意一方还持有写端 fd，
 * 读端就不该看到 EOF
 * ============================================================ */
static void test_forgot_close_write_end(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    long cpid = sys_clone();
    if (cpid == 0)
    {
        /* 子进程"忘了关"继承来的写端 fd[1]，直接用 O_NONBLOCK 读探测 */
        sys_fcntl(fd[0], F_SETFL, O_NONBLOCK);
        char b;
        long r = sys_read(fd[0], &b, 1);
        sys_exit(r == -EAGAIN ? 0 : 1);
    }

    /* 父进程也"忘了关"自己的写端 fd[1]，先等子进程跑完探测 */
    int cst = 0;
    sys_wait4(cpid, &cst);
    check_eq("forgot-to-close write end: reader sees -EAGAIN not EOF",
             (cst >> 8) & 0xff, 0);

    /* 收尾：现在真正关掉，回收 pipe_t */
    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 14：多写者原子性 —— 两个子进程各写一条 100 字节（< PIPE_BUF）的定长
 * 记录，父读出的两条记录必须各自完整、不交错
 * ============================================================ */
static void test_multi_writer_atomicity(void)
{
    int fd[2];
    sys_pipe2(fd, 0);

    long w1 = sys_clone();
    if (w1 == 0)
    {
        static char rec[100];
        for (int i = 0; i < 100; i++)
        {
            rec[i] = 'A';
        }
        long w = sys_write(fd[1], rec, 100);
        sys_exit(w == 100 ? 0 : 1);
    }
    long w2 = sys_clone();
    if (w2 == 0)
    {
        static char rec[100];
        for (int i = 0; i < 100; i++)
        {
            rec[i] = 'B';
        }
        long w = sys_write(fd[1], rec, 100);
        sys_exit(w == 100 ? 0 : 1);
    }

    int st1 = 0;
    int st2 = 0;
    sys_wait4(-1, &st1);
    sys_wait4(-1, &st2);
    check_eq("writer1 wrote its 100B record", (st1 >> 8) & 0xff, 0);
    check_eq("writer2 wrote its 100B record", (st2 >> 8) & 0xff, 0);

    static char buf[200];
    long r = sys_read(fd[0], buf, 200);
    check_eq("parent reads 200 bytes total", r, 200);

    int atomic_ok = (r == 200);
    if (atomic_ok)
    {
        char first = buf[0];
        for (int i = 0; i < 100 && atomic_ok; i++)
        {
            if (buf[i] != first)
            {
                atomic_ok = 0;
            }
        }
        if (atomic_ok)
        {
            char second = buf[100];
            if (second == first)
            {
                atomic_ok = 0; /* 两条记录字符必须不同，否则测不出交错 */
            }
            for (int i = 100; i < 200 && atomic_ok; i++)
            {
                if (buf[i] != second)
                {
                    atomic_ok = 0;
                }
            }
        }
    }
    check("two writers' 100B records did not interleave", atomic_ok);

    sys_close(fd[0]);
    sys_close(fd[1]);
}

/* ============================================================
 * 用例 15：SMP 压测 —— 父子各持一对单向管道双向 ping-pong 20 轮
 * ============================================================ */
static void test_smp_stress(void)
{
    int p2c[2]; /* parent -> child */
    int c2p[2]; /* child -> parent */
    sys_pipe2(p2c, 0);
    sys_pipe2(c2p, 0);

    long spid = sys_clone();
    if (spid == 0)
    {
        int ok = 1;
        for (int round = 0; round < 20; round++)
        {
            char rb[4] = {0};
            if (sys_read(p2c[0], rb, 4) != 4)
            {
                ok = 0;
                break;
            }
            if (sys_write(c2p[1], rb, 4) != 4)
            {
                ok = 0;
                break;
            }
        }
        sys_exit(ok ? 0 : 1);
    }

    int stress_ok = 1;
    for (int round = 0; round < 20; round++)
    {
        char msg[4] = {'p', 'i', 'n', 'g'};
        if (sys_write(p2c[1], msg, 4) != 4)
        {
            stress_ok = 0;
            break;
        }
        char rb[4] = {0};
        if (sys_read(c2p[0], rb, 4) != 4 || ustrcmp(rb, "ping") != 0)
        {
            stress_ok = 0;
            break;
        }
    }

    int sst = 0;
    sys_wait4(spid, &sst);
    check("SMP stress: 20 rounds bidirectional ping-pong ok",
          stress_ok && ((sst >> 8) & 0xff) == 0);

    sys_close(p2c[0]);
    sys_close(p2c[1]);
    sys_close(c2p[0]);
    sys_close(c2p[1]);
}

void _start(void)
{
    puts_fd(1, "\n=== pipetest: Phase 4 pipe syscalls ===\n");

    test_pipe2_basic();
    test_write_read_roundtrip();
    test_eof_after_write_close();
    test_epipe_after_read_close();
    test_wraparound();
    test_blocking_read();
    test_blocking_write();
    test_nonblock();
    test_lseek_espipe();
    test_fstat_fifo();
    test_parent_child_message();
    test_dup_write_end();
    test_forgot_close_write_end();
    test_multi_writer_atomicity();
    test_smp_stress();

    puts_fd(1, "=== pipetest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");

    sys_exit(fail_count == 0 ? 0 : 1);
}
