#include "console.h"
#include "sync.h"
#include "tinyprintf.h"
#include "sbi.h"
#include "tty.h"
#include "uart.h"
#include "memtype.h"
#include "periph_layout.h"

osslock_t ConsoleLock;
volatile bool panicked = false;

/* 内核日志的换行在这里补成回车+换行（同 Linux uart_console_write 的约定）。
 * 以前是 OpenSBI 的 putc 顺手补的、RustSBI 不补，行尾因固件而异；
 * 用户输出的换行转换归 tty 的 ONLCR，不能挪进 uart_putc，否则会补两次。 */
static void stdout_putc(void *unused, char ch)
{
    if (ch == '\n')
    {
        uart_putc('\r');
    }
    uart_putc(ch);
}

/* panic 的逃生通道：不经过自有 UART 驱动，直接交给固件，驱动本身出问题时它仍能说话。 */
static void panic_putc(void *unused, char ch)
{
    sbi_console_putchar((int)ch);
}

void console_init(void)
{
    spinlock_init(&ConsoleLock);
    uart_init(mmu_is_enabled() ? pa_to_kva((phyAddr_t)UART) : (virAddr_t)UART);
    init_printf(0, stdout_putc);
}

void printf(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    irq_key_t ConsoleLock_key = spinlock_acquire(&ConsoleLock);
    tfp_format(NULL, stdout_putc, fmt, args);
    spinlock_release(&ConsoleLock, ConsoleLock_key);
    va_end(args);
}

void panic_impl(const char *func, int line, char *s, ...)
{
    va_list args;
    /* bypass ConsoleLock: panic may be called from within a locked context */
    const char *pre = "\npanic at ";
    for (const char *p = pre; *p; p++) 
    {
        sbi_console_putchar((int)*p);
    }
    for (const char *p = func; *p; p++) 
    {
        sbi_console_putchar((int)*p);
    }
    sbi_console_putchar(':');
    /* 输出行号（十进制） */
    char linebuf[12];
    int i = 0;
    int n = line;
    if (n == 0) 
    {
        linebuf[i++] = '0';
    } 
    else 
    {
        while (n > 0) {
            linebuf[i++] = '0' + (n % 10);
            n /= 10;
        }
        /* 反转 */
        for (int l = 0, r = i - 1; l < r; l++, r--) {
            char tmp = linebuf[l]; linebuf[l] = linebuf[r]; linebuf[r] = tmp;
        }
    }
    for (int j = 0; j < i; j++) 
    {
        sbi_console_putchar((int)linebuf[j]);
    }
    const char *sep = ": ";
    for (const char *p = sep; *p; p++) 
    {
        sbi_console_putchar((int)*p);
    }
    va_start(args, s);
    tfp_format(NULL, panic_putc, s, args);
    va_end(args);
    sbi_console_putchar('\n');
    panicked = true;
    while (true)
        ;
}

/* ============================================================
 * console 设备 file（stdin/stdout/stderr 的后端）
 * ============================================================ */

/* @deprecated 直接转调 tty_open_file()——真正的读写实现已经搬到 tty.c
 * （tty_read 能真正阻塞读到输入，不再是恒返回 EOF 的占位）。保留这个名字只是为了
 * proc_install_stdio() 不用改调用点，下一次大改动时直接把调用点也换成 tty_open_file()，
 * 这个函数连同 console.h 里的声明一起删掉。 */
file_t *console_open_file(void)
{
    return tty_open_file();
}
