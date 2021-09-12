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

void consoleInit(void)
{
    spinlockInit(&ConsoleLock);
    init_printf(0, stdout_putc);
}

//

void printf(char *fmt, ...)
{
    spinlockAcquire(&ConsoleLock);
    tfp_printf(fmt);
    spinlockRelease(&ConsoleLock);
}

void panic(char *s, ...)
{
    char msg[50];
    printf("\npanic: ");
    sprintf(msg, s);
    printf("%s", msg);
    printf("\n");
    /* freeze uart output from other CPUs. */
    panicked = true;
    while (true)
        ;
}