#include "console.h"
#include "cpu.h"
#include "tinyprintf.h"
#include "sbi.h"

#define UNUSED(x) (void)(x)

osslock_t consoleLock;
volatile bool panicked = false;

static void stdout_putc(void *unused, char ch)
{
    sbi_console_putchar((int)ch);
}

void consoleInit(void)
{
    spinlockInit(&consoleLock);
    init_printf(0, stdout_putc);
}

//

void printf(char *fmt, ...)
{
    spinlockAcquire(&consoleLock);
    tfp_printf(fmt);
    spinlockRelease(&consoleLock);
}

void panic(char *s)
{
    printf("panic: ");
    printf(s);
    printf("\n");
    /* freeze uart output from other CPUs. */
    panicked = true; 
    while (true)
        ;
}