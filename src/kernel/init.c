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

static volatile int hart1_up = 0;

/* 此时MMU已打开，并且PC已通过trampoline跳高地址 */
void os_init_after_mmu_enable(uint64_t hartid)
{
    cpu_set_core_id(hartid);
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
        sched_init();   /* 全局就绪队列只能由 hart 0 初始化一次，否则 hart 1 会把 init 冲掉 */
        proc_init();     /* hart 0 的 idle + init */

        /* 启动 hart 1（HSM）。成功则等它过了 trampoline 再移除 trampoline 恒等映射——
         * hart 1 的 trampoline 依赖内核页表里这段恒等映射，提前移除会让它一 csrw satp 就崩。
         * 若 HSM 不支持/失败则退回单核，直接移除。 */
        if (cpu_start_secondary_hart() == SBI_SUCCESS)
        {
            while (!hart1_up)
            {
                mb();
            }
        }
        vmm_remove_identity_mapping();

        printf("core %ld init done\n", cpu_get_core_id());

#if DEBUG_INIT_main_core0
        main(0, (void *)0);
#endif
    }
    else
    {
        hart1_up = 1;   /* 已在高 VA，不再需要恒等映射，放行 hart 0 去移除 */
        mb();
        trap_init();
        tick_init();
        proc_init();
        printf("core %ld init done\n", cpu_get_core_id());
#if DEBUG_INIT_main_core1
        main(0, (void *)0);
#endif
    }

#if DEBUG_INIT_main_bothcore
    main(0, (void *)0);
#endif

#if DEBUG_INIT_os_init
    while (1)
        ;
#endif

    idle(); /* 当前执行上下文成为 idle 线程，永不返回 */
    sbi_shutdown(); /* unreachable */
}
