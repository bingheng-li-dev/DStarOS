#include "sbi.h"
#include "atomic.h"
#include "debug.h"
#include "console.h"
#include "trap.h"
#include "tick.h"
#include "kmalloc.h"
#include "vmm.h"
#include "proc.h"
#include "fs.h"
#include "sync.h"
#include "plic.h"

#ifndef QEMU
#include "sdcard.h"
#include "fpioa.h"
#include "dmac.h"
#endif

#if DEBUG_INIT_main
extern int main(int argc, char **args);
#endif

static void bssInit(void)
{
    extern unsigned int edata;
    extern unsigned int ebss;
    unsigned int *dst;

    dst = &edata;
    while (dst < &ebss)
        *dst++ = 0;
}

void osInit(uint64_t hartid)
{
    setCoreId(hartid);
    if (hartid == 0)
    {
        bssInit();
        consoleInit();
        const char *startmsg = "DStarOS is starting...";
        printf("%s\n", startmsg);
        physicalMemoryManagementInit();
        trapInit();
        tickInit();

#ifndef QEMU
        fpioa_pin_init();
        dmac_init();
#endif
        printf("core %ld init done\n", getCoreId());
#if DEBUG_INIT_main_core0
        main(0, (void *)0);
#endif
        core2Enable();
    }
    else
    {
        mb();
        trapInit();
        tickInit();
        printf("core %ld init done\n", getCoreId());
#if DEBUG_INIT_main_core1
        main(0, (void *)0);
#endif
    }

#if DEBUG_INIT_main_bothcore
    main(0, (void *)0);
#endif
    // plicInit();
    // fs_init();
    // vmm_init();
    // proc_init();
    // idle();

#if DEBUG_INIT_os_init
    while (1)
        ;
#endif

    sbi_shutdown();
}