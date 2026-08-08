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
#include "sched.h"

#ifndef QEMU
#include "sdcard.h"
#include "fpioa.h"
#include "dmac.h"
#endif

#if DEBUG_INIT_main
extern int main(int argc, char **args);
#endif

/* 在 MMU 开启前调用，返回 satp 寄存器值 */
void os_init_before_mmu_enable(void)
{
    console_init();
    printf("DStarOS is starting...\n");
    pmm_init();
    vmm_init();
}

/* 此时MMU已打开，并且PC已通过trampoline跳高地址 */
void os_init_after_mmu_enable(uint64_t hartid)
{
    vmm_remove_identity_mapping();
    setCoreId(hartid);
    if (hartid == 0)
    {
        /* init_printf先前存放了stdout_putc函数的物理绝对地址，更新为高虚拟地址 */
        console_init();
        pmm_init_after_mmu_enable();
        trap_init();
        tick_init();

#ifndef QEMU
        fpioa_pin_init();
        dmac_init();
#endif
        // plicInit();
        fs_init();
        printf("core %ld init done\n", getCoreId());

#if DEBUG_INIT_main_core0
        main(0, (void *)0);
#endif

        core2Enable();
    }
    else
    {
        mb();
        trap_init();
        tick_init();
        printf("core %ld init done\n", getCoreId());
#if DEBUG_INIT_main_core1
        main(0, (void *)0);
#endif
    }
    sched_init();
    proc_init();

#if DEBUG_INIT_main_bothcore
    main(0, (void *)0);
#endif

#if DEBUG_INIT_os_init
    /* 调试断点：停在此处可用 GDB 检查 proc_init 结果；改为 0 以继续运行 idle */
    while (1)
        ;
#endif

    idle(); /* 当前执行上下文成为 idle 线程，永不返回 */
    sbi_shutdown(); /* unreachable */
}
