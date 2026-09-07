/* user/exectest.c —— 验证 dup 与 execve
 *
 * 覆盖两个此前没被真实用户进程触发过的缺口：
 *   1) dup(1) 拿到新 fd，向新 fd 写 → 证明 fd 表 dup 生效（新老 fd 指向同一 console file）；
 *   2) clone 出子进程，子进程 execve("/bin/hello.elf") 变身成另一个程序（打印 "hi"），父进程 wait4 收割
 *      → 证明 do_exec 换脑成功、fd 表在 exec 后仍存活、PID 不变、父子链完整。
 * "/bin/hello.elf" 来自 rootfs 镜像（tools/build_rootfs.sh 在宿主机拷进去的），
 * 不再由内核启动时现写。
 * 不引入 libc，写法与 fork_wait.c 一致。
 */

#define __NR_dup    23
#define __NR_close  57
#define __NR_write  64
#define __NR_exit   93
#define __NR_clone  220
#define __NR_execve 221
#define __NR_wait4  260

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
        /* exit 不应返回；万一返回则原地死循环兜底 */
    }
}

static long sys_dup(int oldfd)          { return syscall4(__NR_dup, oldfd, 0, 0, 0); }
static long sys_close(int fd)           { return syscall4(__NR_close, fd, 0, 0, 0); }
static long sys_clone(void)             { return syscall4(__NR_clone, 0, 0, 0, 0); }
static long sys_wait4(long pid, int *ws){ return syscall4(__NR_wait4, pid, (long)ws, 0, 0); }
static long sys_execve(const char *path){ return syscall4(__NR_execve, (long)path, 0, 0, 0); }

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

void _start(void)
{
    puts_fd(1, "exectest: start\n");

    /* --- 2C：dup --- */
    long fd = sys_dup(1);                       /* 期望返回最小空闲 fd = 3 */
    puts_fd((int)fd, "exectest: hello via dup fd\n");  /* 经 dup 出来的 fd 写；出现即证明 dup 生效 */
    sys_close((int)fd);

    /* --- 2D：clone + execve + wait4 --- */
    long pid = sys_clone();
    if (pid == 0)
    {
        /* 子进程：变身 /hello（打印 "hi\n" 后 exit(0)）。execve 成功则不返回 */
        sys_execve("/bin/hello.elf");
        puts_fd(1, "exectest: EXEC FAILED\n");  /* 只有 execve 失败才会走到这里 */
        sys_exit(1);
    }

    int st = 0;
    sys_wait4(-1, &st);                          /* 收割 exec 后的子进程 */
    puts_fd(1, "exectest: child reaped, done\n");
    sys_exit(0);
}
