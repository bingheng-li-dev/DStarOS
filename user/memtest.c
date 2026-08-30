/* user/memtest.c —— 验证内存管理 syscall（brk / mmap / munmap）
 *
 * 这三个 syscall 的语义全都落在"用户地址空间里能不能真的读写这块地址"上，
 * 内核态自测测不了：brk 的边界由 mm->brk_current 与缺页处理共同决定，
 * mmap 的匿名 VMA 是懒分配的，munmap 是否真回收也只有在用户态再访问一次
 * 才能看出来。所以整套用例全在 U 态跑，写法与 filetest.c/pipetest.c 一致，
 * 不引入 libc。
 *
 * "解除映射后再访问应当触发 fault"这类用例必须在 fork 出的子进程里做——
 * 父进程直接踩会被内核杀掉，整个测试就断在这里了；子进程被杀之后，
 * 父进程 wait4 拿到的退出状态与"子进程自己正常 exit"能区分开，
 * 这就是判据。
 */

#define __NR_write   64
#define __NR_exit    93
#define __NR_brk    214
#define __NR_munmap 215
#define __NR_clone  220
#define __NR_mmap   222
#define __NR_wait4  260

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define EINVAL 22
#define ENOMEM 12
#define ENOSYS 38

#define PAGE_SIZE 4096UL

/* 子进程"访问了不该访问的地址、被内核杀掉"的判据：正常退出的子进程一律
 * exit(CHILD_ALIVE)，被杀的子进程根本走不到那一句。 */
#define CHILD_ALIVE 7

static inline long syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long r_a7 asm("a7") = nr;
    register long r_a0 asm("a0") = a0;
    register long r_a1 asm("a1") = a1;
    register long r_a2 asm("a2") = a2;
    register long r_a3 asm("a3") = a3;
    register long r_a4 asm("a4") = a4;
    register long r_a5 asm("a5") = a5;
    asm volatile("ecall"
                 : "+r"(r_a0)
                 : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5), "r"(r_a7)
                 : "memory");
    return r_a0;
}

static long sys_write(int fd, const void *buf, unsigned long len)
{
    return syscall6(__NR_write, fd, (long)buf, (long)len, 0, 0, 0);
}
static long sys_brk(unsigned long addr)
{
    return syscall6(__NR_brk, (long)addr, 0, 0, 0, 0, 0);
}
static long sys_mmap(unsigned long addr, unsigned long len, int prot, int flags,
                     int fd, unsigned long off)
{
    return syscall6(__NR_mmap, (long)addr, (long)len, prot, flags, fd, (long)off);
}
static long sys_munmap(unsigned long addr, unsigned long len)
{
    return syscall6(__NR_munmap, (long)addr, (long)len, 0, 0, 0, 0);
}
static long sys_clone(void) { return syscall6(__NR_clone, 0, 0, 0, 0, 0, 0); }
static long sys_wait4(long pid, int *ws)
{
    return syscall6(__NR_wait4, pid, (long)ws, 0, 0, 0, 0);
}
static void sys_exit(int code)
{
    syscall6(__NR_exit, code, 0, 0, 0, 0, 0);
    for (;;)
    {
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

/* 往 [base, base+len) 写一串与地址相关的字节再读回，返回 1 表示全部一致。
 * 值与地址挂钩，这样"两块 mmap 区域其实重叠"这种错误也能被检出。 */
static int fill_and_verify(unsigned char *base, unsigned long len, unsigned char seed)
{
    for (unsigned long i = 0; i < len; i++)
    {
        base[i] = (unsigned char)(seed + (i & 0xff));
    }
    for (unsigned long i = 0; i < len; i++)
    {
        if (base[i] != (unsigned char)(seed + (i & 0xff)))
        {
            return 0;
        }
    }
    return 1;
}

/* 在子进程里访问 addr：能访问到就 exit(CHILD_ALIVE)，被内核杀掉则拿不到这个码。
 * 返回 1 表示"这块地址确实还能访问"。 */
static int addr_still_accessible(unsigned long addr)
{
    long pid = sys_clone();
    if (pid == 0)
    {
        volatile unsigned char *p = (volatile unsigned char *)addr;
        *p = 0x5a;
        (void)*p;
        sys_exit(CHILD_ALIVE);
    }
    int ws = 0;
    sys_wait4(pid, &ws);
    return ((ws >> 8) & 0xff) == CHILD_ALIVE;
}

/* ============================================================
 * 组 1：brk
 * ============================================================ */
static void test_brk(void)
{
    long base = sys_brk(0);
    check("brk(0) returns a non-zero page-aligned heap top", base > 0 && (base % (long)PAGE_SIZE) == 0);

    /* 扩张 8 KB 并读写校验：这两页是懒分配的，写第一个字节才真正建立映射 */
    long grown = sys_brk((unsigned long)base + 8192);
    check_eq("brk(base+8192) returns the new brk", grown, base + 8192);
    check("brk grown region is readable/writable",
          fill_and_verify((unsigned char *)base, 8192, 0x11));

    /* 再扩一次，之前写进去的内容不能被扩张动作破坏 */
    long grown2 = sys_brk((unsigned long)base + 16384);
    check_eq("brk can grow twice", grown2, base + 16384);
    int intact = 1;
    for (unsigned long i = 0; i < 8192; i++)
    {
        if (((unsigned char *)base)[i] != (unsigned char)(0x11 + (i & 0xff)))
        {
            intact = 0;
            break;
        }
    }
    check("second brk growth preserves earlier heap contents", intact);

    /* 收缩：物理页必须真的还回去，同时收缩后的地址不再可访问 */
    check_eq("brk shrink back to base", sys_brk((unsigned long)base), base);
    check_eq("brk(0) reports the shrunk value", sys_brk(0), base);
    check("shrunk heap address is no longer accessible",
          !addr_still_accessible((unsigned long)base + 4096));

    /* 失败一律返回旧 brk，不返回负 errno */
    long cur = sys_brk(0);
    check_eq("brk below brk_start is rejected, returns old brk", sys_brk(4096), cur);
    check_eq("brk beyond USER_HEAP_MAX is rejected, returns old brk",
             sys_brk((unsigned long)cur + 0x2000000UL), cur);
    check_eq("brk(0) unchanged after rejected requests", sys_brk(0), cur);

    /* 收缩后重新扩张，拿到的是全新清零页而不是旧内容 */
    check_eq("brk regrow after shrink", sys_brk((unsigned long)cur + 4096), cur + 4096);
    int zeroed = 1;
    for (unsigned long i = 0; i < 4096; i++)
    {
        if (((unsigned char *)cur)[i] != 0)
        {
            zeroed = 0;
            break;
        }
    }
    check("regrown heap page is zero-filled", zeroed);
    sys_brk((unsigned long)cur);
}

/* ============================================================
 * 组 2：mmap
 * ============================================================ */
static void test_mmap(void)
{
    long a = sys_mmap(0, 64 * 1024, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("mmap 64KB anonymous returns a valid address", a > 0);
    check("mmap result is page aligned", (a % (long)PAGE_SIZE) == 0);
    check("mmap region is readable/writable (lazy fault works)",
          fill_and_verify((unsigned char *)a, 64 * 1024, 0x22));

    long b = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    long c = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("three mmaps return distinct non-overlapping regions",
          b > 0 && c > 0 && b != c && (b + 4096 <= c || c + 4096 <= b));
    check("second region independently writable", fill_and_verify((unsigned char *)b, 4096, 0x33));
    check("third region independently writable", fill_and_verify((unsigned char *)c, 4096, 0x44));
    int first_intact = 1;
    for (unsigned long i = 0; i < 64 * 1024; i++)
    {
        if (((unsigned char *)a)[i] != (unsigned char)(0x22 + (i & 0xff)))
        {
            first_intact = 0;
            break;
        }
    }
    check("later mmaps do not clobber the first region", first_intact);

    /* 非页对齐长度要向上取整，1 MB 的大块也要能拿到 */
    long big = sys_mmap(0, 1024 * 1024, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("mmap 1MB succeeds", big > 0);
    check("mmap 1MB region writable at both ends",
          fill_and_verify((unsigned char *)big, 4096, 0x55) &&
          fill_and_verify((unsigned char *)(big + 1024 * 1024 - 4096), 4096, 0x66));

    check_eq("mmap len=0 -> -EINVAL", sys_mmap(0, 0, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0), -EINVAL);
    check_eq("mmap without MAP_ANONYMOUS -> -ENOSYS",
             sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE, -1, 0), -ENOSYS);
    check_eq("mmap with MAP_FIXED -> -EINVAL",
             sys_mmap(0, 4096, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE | MAP_FIXED, -1, 0), -EINVAL);
    check_eq("mmap with fd != -1 -> -EINVAL",
             sys_mmap(0, 4096, PROT_READ, MAP_ANONYMOUS | MAP_PRIVATE, 3, 0), -EINVAL);

    sys_munmap((unsigned long)a, 64 * 1024);
    sys_munmap((unsigned long)b, 4096);
    sys_munmap((unsigned long)c, 4096);
    sys_munmap((unsigned long)big, 1024 * 1024);
}

/* ============================================================
 * 组 3：munmap 的四种覆盖情形
 * ============================================================ */
static void test_munmap(void)
{
    /* 情形 1：完全覆盖 */
    long r = sys_mmap(0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("munmap case1: mmap 4 pages", r > 0);
    fill_and_verify((unsigned char *)r, 4 * PAGE_SIZE, 0x01);
    check_eq("munmap case1: full unmap returns 0", sys_munmap((unsigned long)r, 4 * PAGE_SIZE), 0);
    check("munmap case1: address no longer accessible",
          !addr_still_accessible((unsigned long)r + PAGE_SIZE));

    /* 情形 2：截断头部 */
    r = sys_mmap(0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    fill_and_verify((unsigned char *)r, 4 * PAGE_SIZE, 0x02);
    check_eq("munmap case2: unmap head returns 0", sys_munmap((unsigned long)r, 2 * PAGE_SIZE), 0);
    check("munmap case2: head gone", !addr_still_accessible((unsigned long)r));
    check("munmap case2: tail still there",
          addr_still_accessible((unsigned long)r + 2 * PAGE_SIZE));
    sys_munmap((unsigned long)r + 2 * PAGE_SIZE, 2 * PAGE_SIZE);

    /* 情形 3：截断尾部 */
    r = sys_mmap(0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    fill_and_verify((unsigned char *)r, 4 * PAGE_SIZE, 0x03);
    check_eq("munmap case3: unmap tail returns 0",
             sys_munmap((unsigned long)r + 2 * PAGE_SIZE, 2 * PAGE_SIZE), 0);
    check("munmap case3: head still there", addr_still_accessible((unsigned long)r));
    check("munmap case3: tail gone",
          !addr_still_accessible((unsigned long)r + 3 * PAGE_SIZE));
    sys_munmap((unsigned long)r, 2 * PAGE_SIZE);

    /* 情形 4：中间打洞，VMA 一分为二 */
    r = sys_mmap(0, 4 * PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    fill_and_verify((unsigned char *)r, 4 * PAGE_SIZE, 0x04);
    check_eq("munmap case4: punch a hole in the middle",
             sys_munmap((unsigned long)r + PAGE_SIZE, 2 * PAGE_SIZE), 0);
    check("munmap case4: first page survives", addr_still_accessible((unsigned long)r));
    check("munmap case4: hole page 1 gone",
          !addr_still_accessible((unsigned long)r + PAGE_SIZE));
    check("munmap case4: hole page 2 gone",
          !addr_still_accessible((unsigned long)r + 2 * PAGE_SIZE));
    check("munmap case4: last page survives",
          addr_still_accessible((unsigned long)r + 3 * PAGE_SIZE));
    sys_munmap((unsigned long)r, PAGE_SIZE);
    sys_munmap((unsigned long)r + 3 * PAGE_SIZE, PAGE_SIZE);

    /* 跨多个 VMA 的一次 munmap */
    long m1 = sys_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    long m2 = sys_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    long m3 = sys_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("cross-VMA setup: three adjacent single-page mappings",
          m1 > 0 && m2 == m1 + (long)PAGE_SIZE && m3 == m2 + (long)PAGE_SIZE);
    check_eq("munmap spanning three VMAs returns 0", sys_munmap((unsigned long)m1, 3 * PAGE_SIZE), 0);
    check("cross-VMA: all three gone",
          !addr_still_accessible((unsigned long)m1) &&
          !addr_still_accessible((unsigned long)m2) &&
          !addr_still_accessible((unsigned long)m3));

    /* 地址复用：解除之后 mmap 应当能拿回同一块地址 */
    long reuse = sys_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check_eq("munmap'd address is reused by the next mmap", reuse, m1);
    check("reused region is zero-filled and writable",
          fill_and_verify((unsigned char *)reuse, PAGE_SIZE, 0x07));
    sys_munmap((unsigned long)reuse, PAGE_SIZE);

    check_eq("munmap len=0 -> -EINVAL", sys_munmap((unsigned long)m1, 0), -EINVAL);
    check_eq("munmap unaligned addr -> -EINVAL", sys_munmap((unsigned long)m1 + 1, PAGE_SIZE), -EINVAL);
    check_eq("munmap of never-mapped range is a no-op", sys_munmap((unsigned long)m1, PAGE_SIZE), 0);

    /* 打到堆 VMA 上第一版直接拒绝（brk_current 语义与之冲突）。
     * 必须先把堆撑起来：brk_current 之上的地址在 vmm_vma_get 眼里根本不属于堆 VMA，
     * 那样 munmap 只会当成"这段本来就没映射"直接返回 0。 */
    long heap = sys_brk(0);
    sys_brk((unsigned long)heap + 2 * PAGE_SIZE);
    check_eq("munmap on the heap VMA -> -EINVAL",
             sys_munmap((unsigned long)heap, PAGE_SIZE), -EINVAL);
    sys_brk((unsigned long)heap);
}

/* ============================================================
 * 组 4：mmap 后 fork —— MAP_PRIVATE 的 COW 语义
 * ============================================================ */
static void test_mmap_fork_cow(void)
{
    long r = sys_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check("cow: mmap one page", r > 0);
    volatile unsigned char *p = (volatile unsigned char *)r;
    p[0] = 0xaa;

    long pid = sys_clone();
    if (pid == 0)
    {
        /* 子进程先确认继承到了父进程写下的值，再改成别的 */
        int ok = (p[0] == 0xaa);
        p[0] = 0xbb;
        ok = ok && (p[0] == 0xbb);
        sys_exit(ok ? 0 : 1);
    }
    int ws = 0;
    sys_wait4(pid, &ws);
    check_eq("cow: child inherits the mapping and can write it", (ws >> 8) & 0xff, 0);
    check_eq("cow: parent's copy is unaffected by the child's write", (long)p[0], 0xaa);
    sys_munmap((unsigned long)r, PAGE_SIZE);
}

/* ============================================================
 * 组 5：压力 —— mmap/munmap 交错 200 轮
 * 同时压 vma_cache 的分配释放、slab 空页回收与 syscall 本身；
 * 泄漏的话 mmap 区会被逐渐吃光，最后一轮拿不到地址。
 * ============================================================ */
static void test_stress(void)
{
    int failures = 0;
    unsigned long first = 0;
    for (int round = 0; round < 200; round++)
    {
        unsigned long len = (unsigned long)((round % 4) + 1) * PAGE_SIZE;
        long a = sys_mmap(0, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (a <= 0)
        {
            failures++;
            break;
        }
        if (round == 0)
        {
            first = (unsigned long)a;
        }
        ((unsigned char *)a)[0] = (unsigned char)round;
        ((unsigned char *)a)[len - 1] = (unsigned char)round;
        if (((unsigned char *)a)[0] != (unsigned char)round ||
            ((unsigned char *)a)[len - 1] != (unsigned char)round)
        {
            failures++;
        }
        if (sys_munmap((unsigned long)a, len) != 0)
        {
            failures++;
        }
    }
    check_eq("stress: 200 mmap/munmap rounds without failure", failures, 0);

    /* 没有 VMA 泄漏的话，最后一轮拿到的地址应当回到第一轮的位置 */
    long again = sys_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    check_eq("stress: mmap area fully reclaimed (no VMA leak)", again, (long)first);
    sys_munmap((unsigned long)again, PAGE_SIZE);
}

void _start(void)
{
    puts_fd(1, "\n=== memtest: brk / mmap / munmap ===\n");

    test_brk();
    test_mmap();
    test_munmap();
    test_mmap_fork_cow();
    test_stress();

    puts_fd(1, "=== memtest done: ");
    put_long(pass_count);
    puts_fd(1, " pass  ");
    put_long(fail_count);
    puts_fd(1, " fail ===\n");
    sys_exit(fail_count == 0 ? 0 : 1);
}
