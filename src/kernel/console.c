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
    spinlockInit(&ConsoleLock);
    init_printf(0, stdout_putc);
}

void printf(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    spinlockAcquire(&ConsoleLock);
    tfp_format(NULL, stdout_putc, fmt, args);
    spinlockRelease(&ConsoleLock);
    va_end(args);
}

void panic(char *s, ...)
{
    va_list args;
    va_start(args, s);
    /* bypass ConsoleLock: panic may be called from within a locked context */
    const char *prefix = "\npanic: ";
    for (const char *p = prefix; *p; p++)
        sbi_console_putchar((int)*p);
    tfp_format(NULL, stdout_putc, s, args);
    sbi_console_putchar('\n');
    va_end(args);
    panicked = true;
    while (true)
        ;
}