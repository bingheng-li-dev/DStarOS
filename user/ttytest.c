/* user/ttytest.c —— 验证 TTY 行规范层 + termios/ioctl（Phase 5 Step 9）
 *
 * 与 filetest.c/pipetest.c 同样的理由：TTY 的 read(0,...) 依赖真实用户地址空间指针
 * 与真实异步输入源（QEMU 串口经 sbi_console_getchar() 逐 tick 轮询），只有真正的
 * U 态程序端到端触发才能验证；内核态孤立自测（tty_input_push 的单元测试）测不出
 * "生产者异步、消费者可能跟不上"这类时序问题（Step 5 的 tty_read 多行 bug 就是这样
 * 被发现的，见 .claude/bugfixes.md）。
 *
 * 输入喂法：QEMU -nographic 的 stdin 可以直接管道喂（`printf ... | qemu ...`），
 * 字符不丢、顺序正确（Step 0 调研结论）。全部测试用例的输入按调用顺序拼成一条
 * 输入流，一次性喂给 QEMU；canonical 模式下每次 read 只吐一行（Step 5 已修），
 * raw 模式下用"精确指定 len"的技巧防止一次 read 吞掉后续用例的数据
 * （见 test_raw_mode）。不引入 libc，写法与 filetest.c/pipetest.c 一致。
 */

#define __NR_dup      23
#define __NR_fcntl    25
#define __NR_ioctl    29
#define __NR_openat   56
#define __NR_close    57
#define __NR_lseek    62
#define __NR_read     63
#define __NR_write    64
#define __NR_fstat    80
#define __NR_exit     93
#define __NR_setpgid 154
#define __NR_getpid  172
#define __NR_clone   220
#define __NR_wait4   260

#define AT_FDCWD (-100)

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_NONBLOCK  0x0800

#define SEEK_SET 0

#define F_SETFL 4

/* Linux errno（syscall 失败时返回其负值）*/
#define EAGAIN  11
#define ENOTTY  25
#define ESPIPE  29

#define S_IFMT  0xF000
#define S_IFCHR 0x2000
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)

/* termios/ioctl（asm-generic，与 linux_abi.h 完全一致，字段偏移不可重排）*/
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413

#define SIGINT_NR 2

#define ICANON  0x0002

#define NCCS 19

struct linux_termios
{
    unsigned int  c_iflag;
    unsigned int  c_oflag;
    unsigned int  c_cflag;
    unsigned int  c_lflag;
    unsigned char c_line;
    unsigned char c_cc[NCCS];
};

struct winsize
{
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* Linux riscv64 asm-generic struct stat，128 字节（与 filetest.c/pipetest.c 完全相同）*/
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
static long sys_openat(int dirfd, const char *path, int flags, int mode)
{
    return syscall4(__NR_openat, dirfd, (long)path, flags, mode);
}
static long sys_close(int fd) { return syscall4(__NR_close, fd, 0, 0, 0); }
static long sys_getpid(void) { return syscall4(__NR_getpid, 0, 0, 0, 0); }
static long sys_setpgid(long pid, long pgid)
{
    return syscall4(__NR_setpgid, pid, pgid, 0, 0);
}
static long sys_lseek(int fd, long off, int whence)
{
    return syscall4(__NR_lseek, fd, off, whence, 0);
}
static long sys_fstat(int fd, struct linux_stat *st)
{
    return syscall4(__NR_fstat, fd, (long)st, 0, 0);
}
static long sys_fcntl(int fd, int cmd, long arg)
{
    return syscall4(__NR_fcntl, fd, cmd, arg, 0);
}
static long sys_ioctl(int fd, unsigned long cmd, void *arg)
{
    return syscall4(__NR_ioctl, fd, (long)cmd, (long)arg, 0);
}
static long sys_dup(int oldfd) { return syscall4(__NR_dup, oldfd, 0, 0, 0); }
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

static int umemcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (unsigned long i = 0; i < n; i++)
    {
        if (pa[i] != pb[i])
        {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
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

/* 校验 read() 返回的字节数与内容都符合预期，一次调用顶两条断言
 * （长度和内容分开报告，方便一眼看出是哪一半不对；内容检查复用同一个 name，
 * check() 只是打印用，重名不影响 pass/fail 计数的正确性）*/
static void check_read(const char *name, long got_len, const void *got,
                        long want_len, const void *want)
{
    check_eq(name, got_len, want_len);
    if (got_len == want_len && want_len > 0)
    {
        check(name, umemcmp(got, want, (unsigned long)want_len) == 0);
    }
}

/* ============================================================
 * 用例 1：canonical 基本读
 * ============================================================ */
static void test_canonical_basic(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("canonical basic read", r, buf, 6, "hello\n");
}

/* ============================================================
 * 用例 2：一次 read 不跨行
 * ============================================================ */
static void test_no_cross_line(void)
{
    char buf[64];
    long r1 = sys_read(0, buf, sizeof(buf));
    check_read("no-cross-line: first read", r1, buf, 2, "a\n");
    long r2 = sys_read(0, buf, sizeof(buf));
    check_read("no-cross-line: second read", r2, buf, 2, "b\n");
}

/* ============================================================
 * 用例 3：短读后续读
 * ============================================================ */
static void test_short_read_continue(void)
{
    char buf[64];
    long r1 = sys_read(0, buf, 3);
    check_read("short read: first 3 bytes", r1, buf, 3, "abc");
    long r2 = sys_read(0, buf, sizeof(buf));
    check_read("short read: remaining bytes", r2, buf, 4, "def\n");
}

/* ============================================================
 * 用例 4：\r -> \n（ICRNL）
 * ============================================================ */
static void test_icrnl(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("ICRNL: \\r becomes \\n", r, buf, 3, "hi\n");
}

/* ============================================================
 * 用例 5：退格（DEL 0x7F）
 * ============================================================ */
static void test_backspace_del(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("backspace (DEL) erases last char", r, buf, 3, "ac\n");
}

/* ============================================================
 * 用例 6：^H（0x08）也当退格
 * ============================================================ */
static void test_backspace_ctrlh(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("backspace (^H) erases last char", r, buf, 3, "ac\n");
}

/* ============================================================
 * 用例 7：退格不能越过行首
 * ============================================================ */
static void test_backspace_boundary(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("backspace cannot cross line start", r, buf, 2, "a\n");
}

/* ============================================================
 * 用例 8：^U 杀行
 * ============================================================ */
static void test_kill_line(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("^U kills unterminated line", r, buf, 3, "ok\n");
}

/* ============================================================
 * 用例 9：行首 ^D -> EOF
 * ============================================================ */
static void test_eof_line_start(void)
{
    char buf[8];
    long r = sys_read(0, buf, sizeof(buf));
    check_eq("^D at line start -> EOF (read returns 0)", r, 0);
}

/* ============================================================
 * 用例 10：行中 ^D 提交半行（无 \n）
 * ============================================================ */
static void test_eof_mid_line(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("^D mid-line commits half line (no \\n)", r, buf, 3, "abc");
}

/* ============================================================
 * 用例 11：^D 后还能继续读
 * ============================================================ */
static void test_eof_then_continue(void)
{
    char buf[8];
    long r1 = sys_read(0, buf, sizeof(buf));
    check_eq("^D then continue: first read is EOF", r1, 0);
    long r2 = sys_read(0, buf, sizeof(buf));
    check_read("^D then continue: second read gets next line", r2, buf, 2, "x\n");
}

/* ============================================================
 * 用例 12：阻塞读被唤醒——判据是最终功能结果，不是阻塞机制本身
 * （阻塞/唤醒机制已经由 pipe_test.c 用同一套 waitq 原语精确验证过）
 * ============================================================ */
static void test_blocking_read(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("blocking read eventually gets data", r, buf, 6, "block\n");
}

/* ============================================================
 * 用例 13：O_NONBLOCK
 * ============================================================ */
static void test_nonblock(void)
{
    check_eq("fcntl(0, F_SETFL, O_NONBLOCK) ok", sys_fcntl(0, F_SETFL, O_NONBLOCK), 0);
    char b;
    check_eq("O_NONBLOCK empty read -> -EAGAIN", sys_read(0, &b, 1), -EAGAIN);
    /* 恢复阻塞模式，不影响后续用例 */
    check_eq("fcntl(0, F_SETFL, 0) clears O_NONBLOCK", sys_fcntl(0, F_SETFL, 0), 0);
}

/* ============================================================
 * 用例 14：TCGETS 形状
 * ============================================================ */
static void test_tcgets_shape(void)
{
    struct linux_termios tio;
    check_eq("TCGETS ok", sys_ioctl(0, TCGETS, &tio), 0);
    check("TCGETS: ICANON set by default", (tio.c_lflag & ICANON) != 0);
    check_eq("TCGETS: c_cc[VEOF] == 4 (^D)", tio.c_cc[4], 4);
}

/* ============================================================
 * 用例 15：raw 模式——精确指定 len=2，防止一次 read 吞掉后面用例的数据
 * （raw 模式下 read 不等 \n，只要 avail>0 就返回 min(len,avail)；
 * 全部测试输入是一次性喂给 QEMU 的，此时后续用例的字符可能已经在缓冲区里）
 * ============================================================ */
static void test_raw_mode(void)
{
    struct linux_termios tio;
    check_eq("TCGETS before raw switch", sys_ioctl(0, TCGETS, &tio), 0);
    tio.c_lflag &= ~(unsigned int)ICANON;
    check_eq("TCSETS: turn off ICANON", sys_ioctl(0, TCSETS, &tio), 0);

    /* raw 模式下"不等 \n 就返回"这一点，恰恰意味着一次 read 可能只拿到 1 个字节
     * 就先返回了（尤其 -smp 2 下：hart0 每 push 一个字符就唤醒一次读者，hart1
     * 可能在 hart0 继续 push 第二个字符之前就把读者跑起来了）——这是 raw 模式
     * VMIN=1/VTIME=0 语义本身允许的短读，不是 bug，真实应用读 raw 模式也必须
     * 循环读满，这里同样循环累积到 2 字节，而不是断言"一次 read 就该拿到 2 个"。 */
    char buf[2];
    long got = 0;
    while (got < 2)
    {
        long r = sys_read(0, buf + got, (unsigned long)(2 - got));
        if (r <= 0)
        {
            break;
        }
        got += r;
    }
    check_read("raw mode: read doesn't wait for \\n (accumulated)", got, buf, 2, "xy");
}

/* ============================================================
 * 用例 16：raw -> canonical 切回，验证行为恢复
 * ============================================================ */
static void test_raw_to_canonical(void)
{
    struct linux_termios tio;
    check_eq("TCGETS before canonical switch back", sys_ioctl(0, TCGETS, &tio), 0);
    tio.c_lflag |= (unsigned int)ICANON;
    check_eq("TCSETS: turn ICANON back on", sys_ioctl(0, TCSETS, &tio), 0);

    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("canonical mode restored", r, buf, 2, "z\n");
}

/* ============================================================
 * 用例 17：TIOCGWINSZ
 * ============================================================ */
static void test_tiocgwinsz(void)
{
    struct winsize ws;
    check_eq("TIOCGWINSZ ok", sys_ioctl(0, TIOCGWINSZ, &ws), 0);
    check("TIOCGWINSZ: 24x80", ws.ws_row == 24 && ws.ws_col == 80);
}

/* ============================================================
 * 用例 18：非 TTY 的 fd 调 ioctl -> -ENOTTY
 * ============================================================ */
static void test_ioctl_non_tty(void)
{
    long fd = sys_openat(AT_FDCWD, "/ttytest_tmp.txt", O_CREAT | O_RDWR, 0644);
    check("open regular file for non-tty ioctl test", fd >= 0);
    if (fd < 0)
    {
        return;
    }
    struct linux_termios tio;
    check_eq("TCGETS on regular file -> -ENOTTY", sys_ioctl((int)fd, TCGETS, &tio), -ENOTTY);
    sys_close((int)fd);
}

/* ============================================================
 * 用例 19：lseek(0,...) -> -ESPIPE
 * ============================================================ */
static void test_lseek_espipe(void)
{
    check_eq("lseek(0,...) -> -ESPIPE", sys_lseek(0, 0, SEEK_SET), -ESPIPE);
}

/* ============================================================
 * 用例 20：fstat(0) -> S_ISCHR
 * ============================================================ */
static void test_fstat_ischr(void)
{
    struct linux_stat st;
    check_eq("fstat(0) ok", sys_fstat(0, &st), 0);
    check("fstat(0): S_ISCHR(st_mode)", S_ISCHR(st.st_mode));
}

/* ============================================================
 * 用例 21：/dev/null
 * ============================================================ */
static void test_dev_null(void)
{
    long fd = sys_openat(AT_FDCWD, "/dev/null", O_RDWR, 0);
    check("open /dev/null ok", fd >= 0);
    if (fd < 0)
    {
        return;
    }
    check_eq("write /dev/null returns len", sys_write((int)fd, "abcd", 4), 4);
    char b;
    check_eq("read /dev/null returns 0", sys_read((int)fd, &b, 1), 0);
    sys_close((int)fd);
}

/* ============================================================
 * 用例 22：缓冲溢出不崩——300 字节无 \n 超过 TTY_BUF_SIZE(256)，
 * 多出的字节被安全丢弃（响铃）；^U 杀掉这个卡住的半行腾出空间，
 * 之后新的一行仍能正常读到
 * ============================================================ */
static void test_overflow_no_crash(void)
{
    char buf[64];
    long r = sys_read(0, buf, sizeof(buf));
    check_read("buffer overflow doesn't crash, next line reads fine",
               r, buf, 4, "ok2\n");
}

/* ============================================================
 * 用例 23：dup 后读——得到的 fd 读到同一个 TTY
 * ============================================================ */
static void test_dup_read(void)
{
    long dfd = sys_dup(0);
    check("dup(0) ok", dfd >= 0);
    if (dfd < 0)
    {
        return;
    }
    char buf[64];
    long r = sys_read((int)dfd, buf, sizeof(buf));
    check_read("read via dup'd fd sees same TTY", r, buf, 4, "dup\n");
    sys_close((int)dfd);
}

/* ============================================================
 * 用例 24：fork 后子进程读——fd 0 经 proc_fd_copy 继承
 * ============================================================ */
static void test_fork_read(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        char buf[64];
        long r = sys_read(0, buf, sizeof(buf));
        int ok = (r == 5) && (umemcmp(buf, "fork\n", 5) == 0);
        sys_exit(ok ? 0 : 1);
    }

    int cst = 0;
    sys_wait4(cpid, &cst);
    check_eq("fork: child inherited fd 0 and read the line",
             (cst >> 8) & 0xff, 0);
}

/* ============================================================
 * 用例 25：^C → SIGINT 打给前台进程组
 *
 * 这是唯一能验证"信号从中断上下文产生"这条路径的用例：字符由 tick 中断里的
 * tty_poll_input 收下，signal_send_group 在**持着 tty->lock 的中断上下文**里
 * 只置位 + 唤醒，真正的投递发生在目标自己返回 U 态那一刻。
 *
 * 先把子进程挪进它自己的进程组并设为 tty 前台组，^C 才只打子进程——否则连本
 * 测试进程一起被杀，汇总行都打不出来。子进程带兜底循环，^C 没打中也不会挂死。
 * ============================================================ */
static void test_ctrl_c_kills_foreground(void)
{
    long cpid = sys_clone();
    if (cpid == 0)
    {
        char buf[8];
        /* 阻塞读 stdin，等 ^C 把自己杀掉；打不中时兜底退出，不让测试挂死 */
        for (int i = 0; i < 20; i++)
        {
            sys_read(0, buf, 1);
        }
        sys_exit(9);
    }

    sys_setpgid(cpid, cpid); /* 子进程自成一组 */
    int pg = (int)cpid;
    sys_ioctl(0, TIOCSPGRP, &pg);

    int cst = 0;
    sys_wait4(cpid, &cst);
    check_eq("ctrl-C: foreground child killed by SIGINT", cst & 0x7f, SIGINT_NR);

    /* 前台组还回本进程，免得后续（若有）用例受影响 */
    int self = (int)sys_getpid();
    sys_ioctl(0, TIOCSPGRP, &self);
}

void _start(void)
{
    puts_fd(1, "\n=== ttytest: TTY line discipline + termios/ioctl ===\n");

    test_canonical_basic();
    test_no_cross_line();
    test_short_read_continue();
    test_icrnl();
    test_backspace_del();
    test_backspace_ctrlh();
    test_backspace_boundary();
    test_kill_line();
    test_eof_line_start();
    test_eof_mid_line();
    test_eof_then_continue();
    test_blocking_read();
    test_nonblock();
    test_tcgets_shape();
    test_raw_mode();
    test_raw_to_canonical();
    test_tiocgwinsz();
    test_ioctl_non_tty();
    test_lseek_espipe();
    test_fstat_ischr();
    test_dev_null();
    test_overflow_no_crash();
    test_dup_read();
    test_fork_read();
    test_ctrl_c_kills_foreground();

    puts_fd(1, "=== ttytest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");

    sys_exit(fail_count == 0 ? 0 : 1);
}
