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
#include "uart.h"
#include "sdmmc.h"
#include "sched.h"
#include "tty.h"
#include "slab.h"
#include "ktime.h"
#include "fpu.h"
#include "fdt.h"

#if DEBUG_INIT_main
extern int main(int argc, char **args);
#endif

/* 固件（OpenSBI/RustSBI）或 U-Boot booti 通过 a1 传进来的 DTB 物理地址，
 * 由 startup.S 在 bss 清零之后存入。时基自检与 hart 探测都要读它。 */
uint64_t dtb_phys_addr;

#if DEBUG_BOOT_TRACE
/* 置 1 之后 trap 入口开始记录 scause，见 trap_dispatch() */
int boot_trace_armed;
#endif

/* 在 MMU 开启前调用，返回 satp 寄存器值 */
void os_init_before_mmu_enable(void)
{
    console_init();
    printf("DStarOS is starting...\n");
    sbi_init();
    printf("dtb: phys addr 0x%lx\n", dtb_phys_addr);
    /* 解析一次 DTB，把要用的都读出来。**这三步必须排在 MMU 开启之前**——
     * 一开 MMU，DTB 那块地址就不在内核偏移映射范围内了（VF2 上它甚至在
     * KERNEL_MAP_END 之外）。fdt_init 单独一行而不是藏在下面某个函数里，
     * 是因为下面两个都依赖它，藏起来会变成靠调用顺序维系的隐式约定。 */
    fdt_init((phyAddr_t)dtb_phys_addr);
    tick_check_timebase();
    cpu_probe_harts();
    pmm_init();
    vmm_init();
#if DEBUG_BRINGUP
    vmm_dump_boot_mappings();
    printf("mmu: about to write satp\n");
#endif
}

/* 从核报到标志，按**逻辑 cpu 号**索引；每个从核只写自己那一格，引导核只读，
 * 于是不需要任何原子操作或锁。 */
static volatile int secondary_up[CORE_NUMBER];
/* 等从核报到的自旋上限。取值只要"远大于正常唤醒耗时、又不至于让上不来时干等太久"
 * 即可：QEMU 上实测正常几万圈以内就置位，这里给了三个数量级的余量。 */
#define SECONDARY_UP_SPIN_LIMIT 100000000UL

/* 此时MMU已打开，并且PC已通过trampoline跳高地址。
 * 参数是**逻辑 cpu 号**（引导核恒为 0），不是 hartid——见 startup.S 的说明。 */
void os_init_after_mmu_enable(uint64_t cpu_id)
{
    cpu_set_core_id(cpu_id);
    if (cpu_id == 0)
    {
#if DEBUG_BRINGUP
        /* 这一行**必须绕开 printf**：init_printf 存的 stdout_putc 还是物理地址，
         * 而恒等映射只覆盖 trampoline 那一页，经函数指针调过去当场就 fault。
         * sbi_console_putchar 是直接调用、链接在高 VA，此刻可用。
         * 只让引导核打：这里还没有 ConsoleLock，多核逐字符输出会交错成乱码。 */
        for (const char *p = "mmu: high VA reached\n"; *p != '\0'; p++)
        {
            sbi_console_putchar((int)*p);
        }
#endif
        /* init_printf先前存放了stdout_putc函数的物理绝对地址，更新为高虚拟地址 */
        console_init();
        pmm_init_after_mmu_enable();
        slab_init();
        trap_init();
        fpu_init();   /* 必须早于任何可能执行浮点指令的代码，含 fpu_save/fpu_restore 自身 */
#if DEBUG_MMIO_PROBE
        /* 必须排在 trap_init() 之后：设备区映射不对时，读 LSR 是一发内核缺页，
         * 而 trap_init 之前 stvec 还是 0，表现是**串口完全静默**——与阶段 12 那个
         * A 位 bug 无法区分。放在后面，同一个故障会变成一条带 scause/stval 的 panic。 */
        vmm_probe_mmio();
#endif
#if defined(VF2) && DEBUG_SDMMC_PROBE
        sdmmc_probe();
#endif
#if defined(VF2) && DEBUG_SDMMC_WRITE_TEST
        sdmmc_write_test();
#endif
#if DEBUG_PTE_AD_PROBE
        vmm_probe_pte_ad();
#endif
        tick_init();
        ktime_init();      /* 必须早于任何 ktime_get_ns() 调用；此处 sleeping_tasks/alarm_list 都还是空的 */
        ktime_alarm_init();
        tty_init();      /* 全局 TTY 单例只能由 hart 0 初始化一次；必须早于 proc_init()——
                          * proc_init 会给 init 装 fd 0/1/2，届时 tty 必须已经能用 */
        signal_init();   /* sigpage 的物理页；必须早于任何 create_user_mm() */
#if DEBUG_BOOT_TRACE
        boot_trace_armed = 1;
#endif

        BOOT_TRACE("before plic_init");
        plic_init();          /* 必须晚于 tty_init()：中断一来就会往 tty 里推字符 */
        BOOT_TRACE("after plic_init");
        uart_enable_rx_irq();
        BOOT_TRACE("after uart_enable_rx_irq");
        fs_init();
        BOOT_TRACE("after fs_init");
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
         * 系统已经在多任务调度之后才半路加入，比在启动阶段加入脆弱得多。
         *
         * hart 探测本身已挪到 os_init_before_mmu_enable()——它要读设备树，
         * 而 DTB 只在 MMU 开启之前可访问。 */
        int started = 0;
        for (int id = 1; id < cpu_get_present_count(); id++)
        {
            if (cpu_start_secondary_hart((uint16_t)id) == SBI_SUCCESS)
            {
                started += 1;
            }
        }

        /* 有界等待：SBI 报了 SUCCESS 不代表从核真的活着走到了高 VA。
         * 死等的代价是整机停住、串口再无一个字（这个卡死曾经真实发生过），
         * 有界之后最坏也只是少几个核继续跑，还留下一条可见的日志。 */
        int up = 0;
        for (uint64_t spin = 0; spin < SECONDARY_UP_SPIN_LIMIT && up < started; spin++)
        {
            mb();
            up = 0;
            for (int id = 1; id <= started; id++)
            {
                up += secondary_up[id] ? 1 : 0;
            }
        }
        if (up < started)
        {
            printf("core 0: only %d/%d secondary cpu(s) came up\n", up, started);
        }

        /* 拆 trampoline 恒等映射的前提：**所有已请求启动的从核都已过了 trampoline**。
         * 等超时那一路不能拆——万一某个从核姗姗来迟，trampoline 里一 csrw satp 就跑飞。
         * 留着它只是多占一段内核低半区 VA——用户页表只复制内核高半段 [256..511]，
         * 这段映射对 U 态完全不可见，留着无害。 */
        if (up == started)
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
        /* 已在高 VA，不再需要恒等映射，放行引导核去移除 */
        secondary_up[cpu_id] = 1;
        mb();
        trap_init();
        fpu_init();
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
