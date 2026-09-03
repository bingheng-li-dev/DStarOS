/* user/argvtest.c —— 验证进程启动约定：argc / argv / envp / auxv 初始栈
 *
 * 这个程序有两种被启动的方式，各验一半：
 *   1) 被内核 run_user_program 直接启动（argv = {"/init"}），验证"直接启动的程序
 *      也有合法初始栈"——只给 do_exec 铺 argv 的话，这条路径上 _start 读到的是垃圾；
 *   2) 被 timetest 用 execve("/argvtest", {"/argvtest","one","two"}, {"FOO=bar"})
 *      启动，验证参数与环境真的穿过了 exec 的地址空间切换。
 *
 * 两种方式都验 auxv 与 sp 对齐。全部通过时 exit(0)，否则 exit(失败条数)——
 * 父进程据此判定，不需要管道回传。
 *
 * **_start 必须自己拿到进入时的 sp**：argc/argv/envp/auxv 全在那上面，而编译器
 * 为普通 C 函数生成的序言第一件事就是把 sp 减掉一段。
 */

#define __NR_write 64
#define __NR_exit  93

/* ELF auxiliary vector 的 a_type */
#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_ENTRY   9
#define AT_UID     11
#define AT_CLKTCK  17
#define AT_RANDOM  25

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
    if (a == 0 || b == 0)
    {
        return a == b;
    }
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

/* 在 auxv 里找一个 a_type，找不到返回 notfound */
static unsigned long aux_get(unsigned long *auxv, unsigned long type, unsigned long notfound)
{
    for (unsigned long *p = auxv; p[0] != AT_NULL; p += 2)
    {
        if (p[0] == type)
        {
            return p[1];
        }
    }
    return notfound;
}

void start_c(unsigned long *sp);

/* 拿到未被序言动过的 sp，直接交给 C */
asm(
"   .globl _start\n"
"   .section .text.entry,\"ax\"\n"
"_start:\n"
"   mv   a0, sp\n"
"   call start_c\n"
);

void start_c(unsigned long *sp)
{
    long argc = (long)sp[0];
    char **argv = (char **)&sp[1];
    char **envp = argv + argc + 1;

    /* envp 扫到 NULL，紧随其后就是 auxv 的第一个 a_type——这正是 musl 定位 auxv
     * 的方式，所以 envp 那个结尾 NULL 一旦漏填，下面读到的全是错位的值 */
    unsigned long *auxv = (unsigned long *)envp;
    while (*auxv != 0)
    {
        auxv++;
    }
    auxv++;

    puts_fd(1, "\n=== argvtest: process startup ABI ===\n");
    puts_fd(1, "argc=");
    put_long(argc);
    puts_fd(1, " argv[0]=");
    puts_fd(1, argc > 0 ? argv[0] : "(none)");
    puts_fd(1, "\n");

    expect(((unsigned long)sp & 15UL) == 0, "sp is 16-byte aligned");
    expect(argc >= 1, "argc >= 1");
    expect(argv[argc] == 0, "argv[argc] is NULL");

    if (argc == 1 && ustreq(argv[0], "/init"))
    {
        /* 方式 1：被内核直接启动 */
        puts_fd(1, "-- launched directly by kernel --\n");
        expect(ustreq(argv[0], "/init"), "argv[0] == /init");
        expect(envp[0] == 0, "envp is empty");
    }
    else
    {
        /* 方式 2：被 execve 启动 */
        puts_fd(1, "-- launched via execve --\n");
        expect(argc == 3, "argc == 3");
        expect(ustreq(argv[0], "/argvtest"), "argv[0] == /argvtest");
        expect(argc > 1 && ustreq(argv[1], "one"), "argv[1] == one");
        expect(argc > 2 && ustreq(argv[2], "two"), "argv[2] == two");
        expect(envp[0] != 0 && ustreq(envp[0], "FOO=bar"), "envp[0] == FOO=bar");
        expect(envp[0] != 0 && envp[1] == 0, "envp[1] is NULL");
    }

    /* auxv：AT_PAGESZ 为 0 会让 musl 的 malloc 页对齐算术整个失效，是最关键的一条 */
    unsigned long notfound = 0xdeadbeefUL;
    expect(aux_get(auxv, AT_PAGESZ, notfound) == 4096, "AT_PAGESZ == 4096");
    expect(aux_get(auxv, AT_CLKTCK, notfound) == 100, "AT_CLKTCK == 100");
    expect(aux_get(auxv, AT_UID, notfound) == 0, "AT_UID == 0");
    expect(aux_get(auxv, AT_ENTRY, notfound) != notfound &&
           aux_get(auxv, AT_ENTRY, notfound) != 0, "AT_ENTRY present and non-zero");
    expect(aux_get(auxv, AT_PHENT, notfound) == 56, "AT_PHENT == 56");
    expect(aux_get(auxv, AT_PHNUM, notfound) > 0, "AT_PHNUM > 0");

    /* AT_PHDR 必须落在程序自身的映像里，且程序头表首字节应是一个合法的 p_type。
     * 填了假地址（要么全对要么全 0 那条规矩被破坏）在这里就会现形。 */
    unsigned long phdr = aux_get(auxv, AT_PHDR, notfound);
    expect(phdr != notfound && phdr >= 0x10000 && phdr < 0x20000, "AT_PHDR in image range");
    if (phdr != notfound && phdr >= 0x10000 && phdr < 0x20000)
    {
        unsigned int p_type = *(unsigned int *)phdr;
        expect(p_type <= 7, "phdr[0].p_type looks sane");
    }

    unsigned long rnd = aux_get(auxv, AT_RANDOM, notfound);
    expect(rnd != notfound && rnd != 0, "AT_RANDOM present");
    if (rnd != notfound && rnd != 0)
    {
        unsigned long *r = (unsigned long *)rnd;
        expect((r[0] | r[1]) != 0, "AT_RANDOM seed is non-zero");
    }

    puts_fd(1, "=== argvtest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");

    sys_exit(fail_count);
}
