#include "sbi.h"
#include "debug.h"
#include "tinyprintf.h"
#include "trap.h"
#include "tick.h"
#include "pmm.h"
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

static void stdout_putc(void *unused, char ch)
{
    sbi_console_putchar((int)ch);
}

void os_init(void)
{
    init_bss();
    init_printf(0, stdout_putc);
    const char *startmsg = "os start...";
    printf("%s\n", startmsg);
    trap_init();
    pmm_init();
    tick_init();
    irq_enable();

#if DEBUG_INIT_os_init
    while (1)
        ;
#endif

    sbi_shutdown();
}