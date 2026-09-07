/* user/timetest.c —— 验证阶段 8 的时间、定时器、身份与杂项 syscall
 *
 * 写法与 sigtest.c / memtest.c 一致：不引入 libc，syscall 全部内联 ecall。
 *
 * 时间类用例的判据都必须给足余量：到期检查点是每个 tick 一次（200 Hz，5 ms），
 * 所以"睡 50 ms"实测总会略多于 50 ms，上界要按 tick 粒度 + 调度延迟放宽；
 * 反过来，**下界必须是硬的**（不能早于请求时长返回），那才是真正要测的东西。
 */

#define __NR_set_tid_address 96
#define __NR_nanosleep      101
#define __NR_getitimer      102
#define __NR_setitimer      103
#define __NR_clock_settime  112
#define __NR_clock_gettime  113
#define __NR_clock_getres   114
#define __NR_clock_nanosleep 115
#define __NR_sched_yield    124
#define __NR_rt_sigaction   134
#define __NR_times          153
#define __NR_uname          160
#define __NR_umask          166
#define __NR_gettimeofday   169
#define __NR_write          64
#define __NR_exit           93
#define __NR_getpid         172
#define __NR_gettid         178
#define __NR_getuid         174
#define __NR_geteuid        175
#define __NR_getgid         176
#define __NR_getegid        177
#define __NR_execve         221
#define __NR_clone          220
#define __NR_wait4          260

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME   1

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1

#define SIGALRM 14
#define SA_RESTART 0x10000000UL

#define EINTR  4
#define EINVAL 22

#define NSEC_PER_SEC 1000000000L

struct timespec
{
    long tv_sec;
    long tv_nsec;
};

struct timeval
{
    long tv_sec;
    long tv_usec;
};

struct itimerval
{
    struct timeval it_interval;
    struct timeval it_value;
};

struct tms
{
    long tms_utime;
    long tms_stime;
    long tms_cutime;
    long tms_cstime;
};

#define UTSNAME_LEN 65
struct utsname
{
    char sysname[UTSNAME_LEN];
    char nodename[UTSNAME_LEN];
    char release[UTSNAME_LEN];
    char version[UTSNAME_LEN];
    char machine[UTSNAME_LEN];
    char domainname[UTSNAME_LEN];
};

/* 内核 ABI 的 struct sigaction：24 字节，sa_mask 在最后 */
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
static void sys_exit(int code)
{
    syscall4(__NR_exit, code, 0, 0, 0);
}
static long sys_clock_gettime(int id, struct timespec *ts)
{
    return syscall4(__NR_clock_gettime, id, (long)ts, 0, 0);
}
static long sys_clock_getres(int id, struct timespec *ts)
{
    return syscall4(__NR_clock_getres, id, (long)ts, 0, 0);
}
static long sys_clock_settime(int id, const struct timespec *ts)
{
    return syscall4(__NR_clock_settime, id, (long)ts, 0, 0);
}
static long sys_gettimeofday(struct timeval *tv, void *tz)
{
    return syscall4(__NR_gettimeofday, (long)tv, (long)tz, 0, 0);
}
static long sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
    return syscall4(__NR_nanosleep, (long)req, (long)rem, 0, 0);
}
static long sys_clock_nanosleep(int id, int flags, const struct timespec *req,
                                struct timespec *rem)
{
    return syscall4(__NR_clock_nanosleep, id, flags, (long)req, (long)rem);
}
static long sys_setitimer(int which, const struct itimerval *nv, struct itimerval *ov)
{
    return syscall4(__NR_setitimer, which, (long)nv, (long)ov, 0);
}
static long sys_getitimer(int which, struct itimerval *cv)
{
    return syscall4(__NR_getitimer, which, (long)cv, 0, 0);
}
static long sys_uname(struct utsname *u)
{
    return syscall4(__NR_uname, (long)u, 0, 0, 0);
}
static long sys_umask(int mask)
{
    return syscall4(__NR_umask, mask, 0, 0, 0);
}
static long sys_times(struct tms *t)
{
    return syscall4(__NR_times, (long)t, 0, 0, 0);
}
static long sys_sched_yield(void)
{
    return syscall4(__NR_sched_yield, 0, 0, 0, 0);
}
static long sys_set_tid_address(void *p)
{
    return syscall4(__NR_set_tid_address, (long)p, 0, 0, 0);
}
static long sys_getpid(void)
{
    return syscall4(__NR_getpid, 0, 0, 0, 0);
}
static long sys_gettid(void)
{
    return syscall4(__NR_gettid, 0, 0, 0, 0);
}
static long sys_rt_sigaction(int sig, const struct k_sigaction *act,
                             struct k_sigaction *oact)
{
    return syscall4(__NR_rt_sigaction, sig, (long)act, (long)oact, 8);
}
static long sys_clone(void)
{
    return syscall4(__NR_clone, 0, 0, 0, 0);
}
static long sys_wait4(int pid, int *status, int options)
{
    return syscall4(__NR_wait4, pid, (long)status, options, 0);
}
static long sys_execve(const char *path, char *const *argv, char *const *envp)
{
    return syscall4(__NR_execve, (long)path, (long)argv, (long)envp, 0);
}

static unsigned long ustrlen(const char *s)
{
    unsigned long n = 0;
    while (s && s[n])
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
    char buf[24];
    int i = 0;
    if (v < 0)
    {
        puts_fd(1, "-");
        v = -v;
    }
    if (v == 0)
    {
        buf[i++] = '0';
    }
    while (v > 0)
    {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
    {
        i--;
        sys_write(1, &buf[i], 1);
    }
}
static int ustreq(const char *a, const char *b)
{
    while (*a && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

static int pass_count = 0;
static int fail_count = 0;

static void expect(int cond, const char *name)
{
    if (cond)
    {
        pass_count++;
        puts_fd(1, "  PASS: ");
    }
    else
    {
        fail_count++;
        puts_fd(1, "  FAIL: ");
    }
    puts_fd(1, name);
    puts_fd(1, "\n");
}

/* 单调钟当前值，纳秒 */
static long mono_ns(void)
{
    struct timespec ts;
    sys_clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void sleep_ns(long ns)
{
    struct timespec req = { ns / NSEC_PER_SEC, ns % NSEC_PER_SEC };
    sys_nanosleep(&req, 0);
}

/* 一个 tick 是 5 ms；判据的上界统一按"请求值 + TICK_SLACK"给 */
#define MS  1000000L
#define TICK_SLACK (60 * MS)

/* ---------------- 1. 读时钟 ---------------- */

static void test_clock_gettime(void)
{
    puts_fd(1, "-- clock_gettime / getres / gettimeofday --\n");

    struct timespec a, b;
    expect(sys_clock_gettime(CLOCK_MONOTONIC, &a) == 0, "clock_gettime(MONOTONIC) ok");
    expect(a.tv_nsec >= 0 && a.tv_nsec < NSEC_PER_SEC, "MONOTONIC tv_nsec in range");

    /* 单调：连读两次，后一次不小于前一次，且差值不荒唐 */
    sys_clock_gettime(CLOCK_MONOTONIC, &b);
    long d = (b.tv_sec - a.tv_sec) * NSEC_PER_SEC + (b.tv_nsec - a.tv_nsec);
    expect(d >= 0, "MONOTONIC never goes backwards");
    expect(d < 1 * NSEC_PER_SEC, "two reads are less than 1s apart");

    struct timespec r;
    expect(sys_clock_gettime(CLOCK_REALTIME, &r) == 0, "clock_gettime(REALTIME) ok");
    /* 墙钟起点是 2026-01-01（1767225600），只会往后走 */
    expect(r.tv_sec >= 1767225600L, "REALTIME starts at 2026-01-01 or later");

    struct timespec res;
    expect(sys_clock_getres(CLOCK_MONOTONIC, &res) == 0, "clock_getres ok");
    expect(res.tv_sec == 0 && res.tv_nsec == 100, "resolution is 100 ns (10 MHz timebase)");

    expect(sys_clock_gettime(99, &a) == -EINVAL, "unknown clock id gives EINVAL");

    /* gettimeofday 与 clock_gettime(REALTIME) 必须描述同一个时刻 */
    struct timeval tv;
    expect(sys_gettimeofday(&tv, 0) == 0, "gettimeofday ok");
    sys_clock_gettime(CLOCK_REALTIME, &r);
    long diff = (r.tv_sec - tv.tv_sec) * 1000000L + (r.tv_nsec / 1000 - tv.tv_usec);
    expect(diff >= 0 && diff < 100000L, "gettimeofday agrees with REALTIME (<100ms)");
}

/* ---------------- 2. 设置墙钟 ---------------- */

static void test_clock_settime(void)
{
    puts_fd(1, "-- clock_settime --\n");

    expect(sys_clock_settime(CLOCK_MONOTONIC, 0) == -EINVAL,
           "settime(MONOTONIC) rejected");

    struct timespec bad = { 5, NSEC_PER_SEC };
    expect(sys_clock_settime(CLOCK_REALTIME, &bad) == -EINVAL,
           "settime with tv_nsec >= 1e9 rejected");

    /* 把墙钟拨到一个确定的值，随后读回来应该在它附近；
     * **同一时刻的单调钟不受影响**——这是本组用例的核心。 */
    long mono_before = mono_ns();
    struct timespec want = { 2000000000L, 0 }; /* 2033-05-18 */
    expect(sys_clock_settime(CLOCK_REALTIME, &want) == 0, "settime(REALTIME) ok");

    struct timespec now;
    sys_clock_gettime(CLOCK_REALTIME, &now);
    expect(now.tv_sec >= 2000000000L && now.tv_sec < 2000000002L,
           "REALTIME reflects the new value");

    struct timeval tv;
    sys_gettimeofday(&tv, 0);
    expect(tv.tv_sec >= 2000000000L && tv.tv_sec < 2000000002L,
           "gettimeofday reflects the new value too");

    long mono_after = mono_ns();
    expect(mono_after >= mono_before && mono_after - mono_before < 1 * NSEC_PER_SEC,
           "MONOTONIC unaffected by settime");

    /* 拨完墙钟之后睡眠仍然按真实时长走，不受调时影响 */
    long t0 = mono_ns();
    sleep_ns(50 * MS);
    long spent = mono_ns() - t0;
    expect(spent >= 50 * MS, "sleep after clock jump still lasts >= 50ms");
    expect(spent < 50 * MS + TICK_SLACK, "sleep after clock jump not much longer");

    /* 拨回一个合理的当代时间，免得后面的用例看到奇怪的墙钟 */
    struct timespec back = { 1767225600L, 0 };
    sys_clock_settime(CLOCK_REALTIME, &back);
}

/* ---------------- 3. 睡眠 ---------------- */

static void test_nanosleep(void)
{
    puts_fd(1, "-- nanosleep / clock_nanosleep --\n");

    long t0 = mono_ns();
    struct timespec req = { 0, 50 * MS };
    expect(sys_nanosleep(&req, 0) == 0, "nanosleep(50ms) returns 0");
    long spent = mono_ns() - t0;
    expect(spent >= 50 * MS, "nanosleep slept at least 50ms");
    expect(spent < 50 * MS + TICK_SLACK, "nanosleep did not sleep way too long");

    struct timespec zero = { 0, 0 };
    expect(sys_nanosleep(&zero, 0) == 0, "nanosleep(0) returns immediately");

    struct timespec bad = { 0, NSEC_PER_SEC };
    expect(sys_nanosleep(&bad, 0) == -EINVAL, "tv_nsec >= 1e9 gives EINVAL");
    struct timespec neg = { -1, 0 };
    expect(sys_nanosleep(&neg, 0) == -EINVAL, "negative tv_sec gives EINVAL");

    /* clock_nanosleep 的绝对模式：睡到一个指定的单调时刻 */
    struct timespec now;
    sys_clock_gettime(CLOCK_MONOTONIC, &now);
    long target = now.tv_sec * NSEC_PER_SEC + now.tv_nsec + 50 * MS;
    struct timespec abs = { target / NSEC_PER_SEC, target % NSEC_PER_SEC };
    t0 = mono_ns();
    expect(sys_clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, 0) == 0,
           "clock_nanosleep(ABSTIME) returns 0");
    expect(mono_ns() >= target, "clock_nanosleep(ABSTIME) woke at or after the deadline");
    spent = mono_ns() - t0;
    expect(spent < 50 * MS + TICK_SLACK, "clock_nanosleep(ABSTIME) not much longer");

    /* 已过去的绝对时刻立即返回 */
    struct timespec past = { 0, 0 };
    expect(sys_clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &past, 0) == 0,
           "past deadline returns immediately");

    expect(sys_clock_nanosleep(99, 0, &zero, 0) == -EINVAL,
           "clock_nanosleep with bad clock id gives EINVAL");
}

/* ---------------- 4. 定时器与 SIGALRM ---------------- */

static volatile int alarm_hits = 0;

static void alarm_handler(int sig)
{
    (void)sig;
    alarm_hits++;
}

static void install_alarm_handler(void)
{
    struct k_sigaction sa;
    sa.sa_handler = (unsigned long)alarm_handler;
    sa.sa_flags = 0; /* 不带 SA_RESTART：要让 nanosleep 真的被打断 */
    sa.sa_mask = 0;
    sys_rt_sigaction(SIGALRM, &sa, 0);
}

static void test_itimer(void)
{
    puts_fd(1, "-- setitimer / getitimer / SIGALRM --\n");

    install_alarm_handler();

    expect(sys_setitimer(ITIMER_VIRTUAL, 0, 0) == -EINVAL, "ITIMER_VIRTUAL rejected");
    expect(sys_getitimer(ITIMER_VIRTUAL, 0) == -EINVAL, "getitimer(VIRTUAL) rejected");

    /* 单次 100 ms */
    alarm_hits = 0;
    struct itimerval nv;
    nv.it_interval.tv_sec = 0;
    nv.it_interval.tv_usec = 0;
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 100000; /* 100 ms */
    expect(sys_setitimer(ITIMER_REAL, &nv, 0) == 0, "setitimer(100ms) ok");

    struct itimerval cv;
    expect(sys_getitimer(ITIMER_REAL, &cv) == 0, "getitimer ok");
    expect(cv.it_value.tv_sec == 0 && cv.it_value.tv_usec > 0 &&
           cv.it_value.tv_usec <= 100000, "getitimer reports remaining time");
    long first = cv.it_value.tv_usec;
    sleep_ns(30 * MS);
    sys_getitimer(ITIMER_REAL, &cv);
    expect(cv.it_value.tv_usec < first, "remaining time counts down");

    /* 等它响 */
    long deadline = mono_ns() + 500 * MS;
    while (alarm_hits == 0 && mono_ns() < deadline)
    {
        sleep_ns(5 * MS);
    }
    expect(alarm_hits == 1, "SIGALRM delivered exactly once");

    /* 响过之后定时器应已解除 */
    sys_getitimer(ITIMER_REAL, &cv);
    expect(cv.it_value.tv_sec == 0 && cv.it_value.tv_usec == 0,
           "one-shot timer is disarmed after firing");

    /* 取消：装一个 500 ms 的然后立刻关掉，等够时间也不该响 */
    alarm_hits = 0;
    nv.it_value.tv_usec = 500000;
    sys_setitimer(ITIMER_REAL, &nv, 0);
    struct itimerval off;
    off.it_interval.tv_sec = 0;
    off.it_interval.tv_usec = 0;
    off.it_value.tv_sec = 0;
    off.it_value.tv_usec = 0;
    struct itimerval old;
    expect(sys_setitimer(ITIMER_REAL, &off, &old) == 0, "cancel timer ok");
    expect(old.it_value.tv_usec > 0, "old value reports the remaining time");
    sleep_ns(300 * MS);
    expect(alarm_hits == 0, "cancelled timer does not fire");

    /* 周期定时器：50 ms 一次，等它响 3 次 */
    alarm_hits = 0;
    nv.it_interval.tv_sec = 0;
    nv.it_interval.tv_usec = 50000;
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 50000;
    sys_setitimer(ITIMER_REAL, &nv, 0);
    deadline = mono_ns() + 2 * NSEC_PER_SEC;
    while (alarm_hits < 3 && mono_ns() < deadline)
    {
        sleep_ns(5 * MS);
    }
    expect(alarm_hits >= 3, "periodic timer fired at least 3 times");
    sys_setitimer(ITIMER_REAL, &off, 0);

    /* getitimer 也要如实报告周期 */
    nv.it_interval.tv_usec = 70000;
    nv.it_value.tv_usec = 500000;
    sys_setitimer(ITIMER_REAL, &nv, 0);
    sys_getitimer(ITIMER_REAL, &cv);
    expect(cv.it_interval.tv_usec == 70000, "getitimer reports the interval");
    sys_setitimer(ITIMER_REAL, &off, 0);
}

/* ---------------- 5. 睡眠被信号打断 ---------------- */

static void test_sleep_interrupted(void)
{
    puts_fd(1, "-- nanosleep interrupted by SIGALRM --\n");

    install_alarm_handler();
    alarm_hits = 0;

    /* 50 ms 后来一发 SIGALRM，打断一个 2 秒的睡眠 */
    struct itimerval nv;
    nv.it_interval.tv_sec = 0;
    nv.it_interval.tv_usec = 0;
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 50000;
    sys_setitimer(ITIMER_REAL, &nv, 0);

    struct timespec req = { 2, 0 };
    struct timespec rem = { -1, -1 };
    long t0 = mono_ns();
    long ret = sys_nanosleep(&req, &rem);
    long spent = mono_ns() - t0;

    expect(ret == -EINTR, "interrupted nanosleep returns EINTR");
    expect(alarm_hits == 1, "handler ran");
    expect(spent < 1 * NSEC_PER_SEC, "returned early, well before the full 2s");
    expect(rem.tv_sec >= 0 && rem.tv_nsec >= 0 && rem.tv_nsec < NSEC_PER_SEC,
           "rem is a well-formed timespec");
    long remain = rem.tv_sec * NSEC_PER_SEC + rem.tv_nsec;
    expect(remain > 0 && remain < 2 * NSEC_PER_SEC, "rem is less than the request");
    /* rem 必须与实际耗时对得上——恒填 0 或填成请求值都会在这里露馅 */
    long total = spent + remain;
    expect(total >= 2 * NSEC_PER_SEC - TICK_SLACK &&
           total < 2 * NSEC_PER_SEC + TICK_SLACK, "spent + rem is about 2s");

    /* TIMER_ABSTIME 下按 POSIX 不回填 rem */
    alarm_hits = 0;
    nv.it_value.tv_usec = 50000;
    sys_setitimer(ITIMER_REAL, &nv, 0);
    struct timespec now;
    sys_clock_gettime(CLOCK_MONOTONIC, &now);
    long target = now.tv_sec * NSEC_PER_SEC + now.tv_nsec + 2 * NSEC_PER_SEC;
    struct timespec abs = { target / NSEC_PER_SEC, target % NSEC_PER_SEC };
    struct timespec untouched = { 12345, 6789 };
    ret = sys_clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, &untouched);
    expect(ret == -EINTR, "interrupted clock_nanosleep(ABSTIME) returns EINTR");
    expect(untouched.tv_sec == 12345 && untouched.tv_nsec == 6789,
           "ABSTIME does not write rem");
}

/* ---------------- 6. 定时器与睡眠互不干扰 ---------------- */

static void test_timer_and_sleep_coexist(void)
{
    puts_fd(1, "-- itimer and nanosleep on two independent lists --\n");

    /* 一个进程同时"睡在 nanosleep 里"和"装着定时器"，两条链表必须互不干扰。
     * 共用一个链表节点的话，这里就是链表损坏。 */
    install_alarm_handler();
    alarm_hits = 0;

    struct itimerval nv;
    nv.it_interval.tv_sec = 0;
    nv.it_interval.tv_usec = 0;
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 300000; /* 300 ms，比下面的睡眠长 */
    sys_setitimer(ITIMER_REAL, &nv, 0);

    long t0 = mono_ns();
    sleep_ns(80 * MS);
    long spent = mono_ns() - t0;
    expect(spent >= 80 * MS && spent < 80 * MS + TICK_SLACK,
           "sleep completes normally while a timer is armed");
    expect(alarm_hits == 0, "timer has not fired yet");

    long deadline = mono_ns() + 1 * NSEC_PER_SEC;
    while (alarm_hits == 0 && mono_ns() < deadline)
    {
        sleep_ns(5 * MS);
    }
    expect(alarm_hits == 1, "timer still fires after the sleep completed");
}

/* ---------------- 7. 身份与杂项 ---------------- */

static void test_uname(void)
{
    puts_fd(1, "-- uname --\n");
    struct utsname u;
    for (unsigned long i = 0; i < sizeof(u); i++)
    {
        ((char *)&u)[i] = (char)0xAA; /* 先涂脏，验证内核确实把 390 字节都写了 */
    }
    expect(sys_uname(&u) == 0, "uname ok");
    expect(ustreq(u.sysname, "DStarOS"), "sysname == DStarOS");
    expect(ustreq(u.machine, "riscv64"), "machine == riscv64");
    expect(ustrlen(u.nodename) > 0, "nodename non-empty");
    expect(ustrlen(u.release) > 0, "release non-empty");
    expect(ustrlen(u.version) > 0, "version non-empty");
    /* domainname 是最容易漏掉的一个字段——漏掉的话这里还是 0xAA */
    expect(ustrlen(u.domainname) > 0 && ustrlen(u.domainname) < UTSNAME_LEN,
           "domainname filled in (not left as garbage)");
}

static void test_ids(void)
{
    puts_fd(1, "-- ids / umask / set_tid_address / sched_yield --\n");
    expect(syscall4(__NR_getuid, 0, 0, 0, 0) == 0, "getuid == 0");
    expect(syscall4(__NR_geteuid, 0, 0, 0, 0) == 0, "geteuid == 0");
    expect(syscall4(__NR_getgid, 0, 0, 0, 0) == 0, "getgid == 0");
    expect(syscall4(__NR_getegid, 0, 0, 0, 0) == 0, "getegid == 0");
    expect(sys_gettid() == sys_getpid(), "gettid == getpid (no thread groups)");

    long old = sys_umask(0022);
    expect(old >= 0, "umask returns the old mask");
    expect(sys_umask(0077) == 0022, "umask returns what the previous call set");
    sys_umask(old);

    long tid = 0;
    expect(sys_set_tid_address(&tid) == sys_getpid(), "set_tid_address returns pid");
    expect(sys_sched_yield() == 0, "sched_yield returns 0");
}

static void test_times(void)
{
    puts_fd(1, "-- times --\n");
    struct tms t1, t2;
    long r1 = sys_times(&t1);
    expect(r1 > 0, "times returns a positive tick count");
    expect(t1.tms_utime >= 0, "tms_utime is non-negative");
    expect(t1.tms_cutime == 0, "tms_cutime is 0 before reaping any child");

    sleep_ns(30 * MS);
    long r2 = sys_times(&t2);
    expect(r2 >= r1, "times return value is monotonic");
    expect(t2.tms_utime >= t1.tms_utime, "tms_utime never decreases");
    /* 睡了 30 ms，100 Hz 下返回值至少该涨 2 格 */
    expect(r2 - r1 >= 2, "times advanced by at least 2 ticks over a 30ms sleep");

    /* fork 一个烧 CPU 的子进程，收割之后 tms_cutime 应变成非 0。
     * **按墙钟烧够 100 ms 而不是固定圈数**：tms_cutime 的单位是 100 Hz（10 ms 一格），
     * 固定圈数在快一点的机器上只够 10 ms 出头，整数除法一截断就是 0——判据会正好
     * 压在截断边界上，随机失败。按时间烧则与 QEMU 快慢无关。 */
    long pid = sys_clone();
    if (pid == 0)
    {
        volatile long x = 0;
        long stop = mono_ns() + 100 * MS;
        while (mono_ns() < stop)
        {
            for (long i = 0; i < 100000; i++)
            {
                x += i;
            }
        }
        sys_exit(0);
    }
    int status = 0;
    sys_wait4((int)pid, &status, 0);
    struct tms t3;
    sys_times(&t3);
    expect(t3.tms_cutime > 0, "tms_cutime is non-zero after reaping a busy child");
    expect(t3.tms_cstime == 0, "tms_cstime stays 0 (no user/sys split)");
}

/* ---------------- 8. fork / exec 的定时器与 umask 语义 ---------------- */

static void test_fork_exec_semantics(void)
{
    puts_fd(1, "-- fork does not inherit the timer; umask does --\n");

    install_alarm_handler();
    alarm_hits = 0;

    struct itimerval nv;
    nv.it_interval.tv_sec = 0;
    nv.it_interval.tv_usec = 0;
    nv.it_value.tv_sec = 0;
    nv.it_value.tv_usec = 100000;
    sys_umask(0055);
    sys_setitimer(ITIMER_REAL, &nv, 0);

    long pid = sys_clone();
    if (pid == 0)
    {
        /* 子进程：定时器应为空、umask 应继承 */
        struct itimerval cv;
        sys_getitimer(ITIMER_REAL, &cv);
        int ok = (cv.it_value.tv_sec == 0 && cv.it_value.tv_usec == 0);
        ok = ok && (sys_umask(0) == 0055);
        sys_exit(ok ? 0 : 1);
    }
    int status = 0;
    sys_wait4((int)pid, &status, 0);
    expect(((status >> 8) & 0xff) == 0,
           "child: timer cleared by fork, umask inherited");
    sys_umask(0022);

    /* 父进程自己的定时器不受影响，仍然会响 */
    long deadline = mono_ns() + 1 * NSEC_PER_SEC;
    while (alarm_hits == 0 && mono_ns() < deadline)
    {
        sleep_ns(5 * MS);
    }
    expect(alarm_hits == 1, "parent timer still fires after fork");
}

/* ---------------- 9. argv / envp / auxv ---------------- */

static void test_argv_envp(void)
{
    puts_fd(1, "-- execve with argv/envp (see argvtest output below) --\n");

    static char a0[] = "/argvtest";
    static char a1[] = "one";
    static char a2[] = "two";
    static char e0[] = "FOO=bar";
    static char *argv[] = { a0, a1, a2, 0 };
    static char *envp[] = { e0, 0 };

    long pid = sys_clone();
    if (pid == 0)
    {
        sys_execve("/bin/argvtest.elf", argv, envp);
        sys_exit(99); /* 只有 execve 失败才会走到这里 */
    }
    int status = 0;
    sys_wait4((int)pid, &status, 0);
    int code = (status >> 8) & 0xff;
    expect(code != 99, "execve(/bin/argvtest.elf) succeeded");
    expect(code == 0, "argvtest reported 0 failures");
}

void _start(void)
{
    puts_fd(1, "\n=== timetest: time & misc syscalls ===\n");

    test_clock_gettime();
    test_clock_settime();
    test_nanosleep();
    test_itimer();
    test_sleep_interrupted();
    test_timer_and_sleep_coexist();
    test_uname();
    test_ids();
    test_times();
    test_fork_exec_semantics();
    test_argv_envp();

    puts_fd(1, "=== timetest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");

    sys_exit(fail_count == 0 ? 0 : 1);
}
