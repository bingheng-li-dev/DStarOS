/* user/segtest.c —— 验证 elf_load 正确处理"多个 PT_LOAD 共享同一物理页"的 ELF
 *
 * 本程序**必须用 user/user_dense.ld + -z max-page-size=16 链接**（见 user/Makefile），
 * 那份脚本刻意去掉了 user.ld 里的段间 ALIGN(4096)，于是 RX 段与 RW 段的虚拟地址
 * 首尾相接、共用中间那一页。用默认的 user.ld 链出来的话两段各自页对齐，
 * 这些断言全都退化成恒真，等于没测。
 *
 * 为什么要专门造这个形状：真实工具链链出来的可执行文件（含 musl 链的 BusyBox）
 * 是否共享页，取决于链接器的 -z separate-code 之类的默认行为，内核不能依赖它。
 * 段是文件的单位、页是地址空间的单位，elf_load 必须自己把两者对上。
 *
 * 三块数据分别落在三处，覆盖损坏的三种表现：
 *   ro[] 在 RX 段尾部，跨过共享页——每页只映一次没做对的话，这块会被后一段
 *        新分配的零页整片盖掉，是最主要的判据；
 *   rw[] 在 RW 段头部，也在共享页里，验证后一段自己的内容没被写飞；
 *   bs[] 在 .bss，验证 p_memsz > p_filesz 的清零range没有越界写到邻居头上。
 *
 * 三个数组本身**不加 volatile**，否则 GCC 会把 `const volatile` 当成"可能被外部改"
 * 而放进 .data，ro[] 就跑到 RW 段里去了，共享页改落在 .text 上——那样程序进不了
 * 第一条指令就崩，反而看不到是哪块数据坏了。防常量折叠改由读取侧负责：
 * 所有访问都through一个 volatile 限定的指针，volatile 访存不允许被折叠掉。
 */

#define __NR_write 64
#define __NR_exit  93

#define RO_SIZE 5000
#define RW_SIZE 2000
#define BS_SIZE 2000

#define RO_BYTE 0x5a
#define RW_BYTE 0xa5

static const unsigned char ro[RO_SIZE] = {[0 ... RO_SIZE - 1] = RO_BYTE};
static unsigned char rw[RW_SIZE]       = {[0 ... RW_SIZE - 1] = RW_BYTE};
static unsigned char bs[BS_SIZE];

static inline long syscall3(long nr, long a0, long a1, long a2)
{
    register long r_a7 asm("a7") = nr;
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    asm volatile("ecall"
                 : "+r"(r_a0)
                 : "r"(r_a1), "r"(r_a2), "r"(r_a7)
                 : "memory");
    return r_a0;
}

static long sys_write(int fd, const void *buf, unsigned long len)
{
    return syscall3(__NR_write, fd, (long)buf, (long)len);
}

static void sys_exit(int code)
{
    syscall3(__NR_exit, code, 0, 0);
    for (;;)
    {
    }
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

/* 返回第一个不等于 want 的下标，全部相等时返回 -1。
 * 返回下标而不是布尔值，是为了让失败时能直接看出损坏从哪个偏移开始——
 * 整片被零页盖掉与个别字节写错，现象完全不同。 */
static long first_mismatch(const volatile unsigned char *p, unsigned long n, unsigned char want)
{
    for (unsigned long i = 0; i < n; i++)
    {
        if (p[i] != want)
        {
            return (long)i;
        }
    }
    return -1;
}

static void report_mismatch(const char *name, long idx)
{
    puts_fd(1, "    ");
    puts_fd(1, name);
    puts_fd(1, " first mismatch at index ");
    put_long(idx);
    puts_fd(1, "\n");
}

static int main(void)
{
    puts_fd(1, "=== segtest: shared-page PT_LOAD ===\n");

    /* 三块数据的地址一并打出来：共享的是哪一页，看这三个值最直接。 */
    puts_fd(1, "  ro=0x");
    put_long((long)(unsigned long)&ro[0]);
    puts_fd(1, " rw=0x");
    put_long((long)(unsigned long)&rw[0]);
    puts_fd(1, " bs=0x");
    put_long((long)(unsigned long)&bs[0]);
    puts_fd(1, "\n");

    long idx = first_mismatch(ro, RO_SIZE, RO_BYTE);
    expect(idx < 0, "RX segment .rodata intact across shared page");
    if (idx >= 0)
    {
        report_mismatch("ro", idx);
    }

    idx = first_mismatch(rw, RW_SIZE, RW_BYTE);
    expect(idx < 0, "RW segment .data intact");
    if (idx >= 0)
    {
        report_mismatch("rw", idx);
    }

    idx = first_mismatch(bs, BS_SIZE, 0);
    expect(idx < 0, "BSS zeroed");
    if (idx >= 0)
    {
        report_mismatch("bs", idx);
    }

    /* 共享页必须真的可写：合并后权限取并集，若错取成 RX 段的只读权限，
     * 这一笔会触发 store page fault 直接被杀，跑不到下面的汇总行。 */
    volatile unsigned char *w = rw;
    w[0]                      = 0x11;
    w[RW_SIZE - 1]            = 0x22;
    expect(w[0] == 0x11 && w[RW_SIZE - 1] == 0x22, "shared page is writable");

    puts_fd(1, "=== segtest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");
    return fail_count;
}

void _start(void)
{
    sys_exit(main());
}
