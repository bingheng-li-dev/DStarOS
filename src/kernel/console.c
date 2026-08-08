#include "console.h"
#include "sync.h"
#include "tinyprintf.h"
#include "sbi.h"

#define UNUSED(x) (void)(x)

osslock_t ConsoleLock;
volatile bool panicked = false;

static void stdout_putc(void *unused, char ch)
{
    sbi_console_putchar((int)ch);
}

void console_init(void)
{
    spinlock_init(&ConsoleLock);
    init_printf(0, stdout_putc);
}

void printf(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    spinlock_acquire(&ConsoleLock);
    tfp_format(NULL, stdout_putc, fmt, args);
    spinlock_release(&ConsoleLock);
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
    tfp_format(NULL, stdout_putc, s, args);
    va_end(args);
    sbi_console_putchar('\n');
    panicked = true;
    while (true)
        ;
}