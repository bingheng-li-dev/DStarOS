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
#include "tty.h"
#include "slab.h"

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
/* 等 hart 1 报到的自旋上限。取值只要"远大于正常唤醒耗时、又不至于让上不来时干等太久"
 * 即可：QEMU 上实测正常几万圈以内就置位，这里给了三个数量级的余量。 */
#define HART1_UP_SPIN_LIMIT 100000000UL

/* 此时MMU已打开，并且PC已通过trampoline跳高地址 */
void os_init_after_mmu_enable(uint64_t hartid)
{
    cpu_set_core_id(hartid);
    if (hartid == 0)
    {
        /* init_printf先前存放了stdout_putc函数的物理绝对地址，更新为高虚拟地址 */
        console_init();
        pmm_init_after_mmu_enable();
        slab_init();
        trap_init();
#if DEBUG_PTE_AD_PROBE
        vmm_probe_pte_ad();
#endif
        tick_init();
        tty_init();      /* 全局 TTY 单例只能由 hart 0 初始化一次；必须早于 proc_init()——
                          * proc_init 会给 init 装 fd 0/1/2，届时 tty 必须已经能用 */
        signal_init();   /* sigpage 的物理页；必须早于任何 create_user_mm() */

#ifndef QEMU
        fpioa_pin_init();
        dmac_init();
#endif
        // plicInit();
        fs_init();
        sched_init();   /* 全局就绪队列只能由 hart 0 初始化一次，否则 hart 1 会把 init 冲掉 */
        proc_early_init(); /* proc_list / proc_list_lock / pid_stack，必须早于启动 hart 1 */

        /* 启动 hart 1（HSM）。成功则等它过了 trampoline 再移除 trampoline 恒等映射——
         * hart 1 的 trampoline 依赖内核页表里这段恒等映射，提前移除会让它一 csrw satp 就崩。
         * 若 HSM 不支持/失败则退回单核，直接移除。
         *
         * **这一段必须排在 proc_init() 之前**（2026-09-01 修）。原来排在后面，而
         * proc_init() 里 fork 出 init 时 sched_activate() 会当场把本执行流（此时已经
         * 是 hart0 的 idle 任务）抢占掉，剩下的启动代码要等 idle 被重新调度才继续——
         * 实测 30 次里只有 3 次轮得上，也就是**九成的运行里 hart 1 根本没启动、
         * 整个系统是单核跑的**，`-smp 2` 形同虚设。少数轮得上的运行里，hart 1 又是在
         * 系统已经在多任务调度之后才半路加入，比在启动阶段加入脆弱得多。 */
        bool hsm_ok = (cpu_start_secondary_hart() == SBI_SUCCESS);
        bool hart1_ok = false;
        if (hsm_ok)
        {
            /* 有界等待：SBI 报了 SUCCESS 不代表 hart 1 真的活着走到了高 VA。
             * 死等的代价是整机停住、串口再无一个字（这个卡死曾经真实发生过），
             * 有界之后最坏也只是退回单核继续跑，还留下一条可见的日志。 */
            for (uint64_t spin = 0; spin < HART1_UP_SPIN_LIMIT && !hart1_up; spin++)
            {
                mb();
            }
            hart1_ok = (hart1_up != 0);
            if (!hart1_ok)
            {
                printf("core 0: hart 1 did not come up, falling back to single core\n");
            }
        }
        /* 拆恒等映射的两个前提，满足其一即可：hart 1 已经过了 trampoline（hart1_ok），
         * 或者 HSM 压根没启动成功、hart 1 永远不会来（!hsm_ok）。
         * **等超时那一路不能拆**：万一 hart 1 姗姗来迟，trampoline 里一 csrw satp 就跑飞。
         * 留着它只是多占一段内核低半区 VA——用户页表只复制内核高半段 [256..511]，
         * 这段映射对 U 态完全不可见，留着无害。 */
        if (hart1_ok || !hsm_ok)
        {
            vmm_remove_identity_mapping();
        }

        printf("core %ld init done\n", cpu_get_core_id());

        proc_init();     /* hart 0 的 idle + init；放在最后，此后被抢占都无所谓 */

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
