#include "sbi.h"
#include "debug.h"
#include "tinyprintf.h"
#include "trap.h"
#include "tick.h"
#define UNUSED(x) (void)(x)

static void init_bss(void)
{
    extern unsigned int edata;
    extern unsigned int ebss;
    unsigned int *dst;

    dst = &edata;
    while (dst < &ebss)
        *dst++ = 0;
}

static void stdout_putc(void *unused, char *ch)
{
    sbi_console_putchar(ch);
}

void os_init(void)
{
    init_bss();
    init_printf(0, stdout_putc);
    const char *startmsg = "os start...";
    printf("%s\n", startmsg);
    trap_init();
#if DEBUG
    printf("trap inited!\n");
#endif
    tick_init();
    irq_enable();
    while (1)
    {
        ;
    }
    
    sbi_shutdown();
}