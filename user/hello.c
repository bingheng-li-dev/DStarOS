/* user/hello.c —— 用户态构建流水线的测试程序
 * 不引入 libc，自带 _start 和内联 ecall 系统调用包装。
 * 行为与之前手写汇编版 user_hello.S 完全一致：write(1, "hi\n", 3); exit(0)。
 */

#define __NR_write 64
#define __NR_exit  93

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

static long sys_write(int fd, const char *buf, unsigned long len)
{
    return syscall3(__NR_write, fd, (long)buf, (long)len);
}

static void sys_exit(int code)
{
    syscall3(__NR_exit, code, 0, 0);
    for (;;)
    {
        /* exit 系统调用不应返回；万一返回则原地死循环兜底 */
    }
}

static int main(void)
{
    const char msg[] = "hi\n";
    sys_write(1, msg, 3);
    return 0;
}

/* 程序入口：由链接脚本 ENTRY(_start) 指定，elf_load 从 ELF 头 e_entry 取得此地址。
 * 普通 C 函数即可胜任——只要保证它不会真的执行到末尾的 ret（sys_exit 内部死循环兜底），
 * 进入时的 sp 由内核 enter_user_mode 提前设好，满足 psABI 的 16 字节对齐要求。 */
void _start(void)
{
    int code = main();
    sys_exit(code);
}
