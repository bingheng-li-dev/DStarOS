/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

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
#include "probes.h"
#include "startup.h"

/* 固件（OpenSBI/RustSBI）或 U-Boot booti 通过 a1 传进来的 DTB 物理地址，
 * 由 startup.S 在 bss 清零之后存入。时基自检与 hart 探测都要读它。 */
uint64_t dtb_phys_addr;

#if DEBUG_BOOT_TRACE
/* 置 1 之后 trap 入口开始记录 scause，见 trap_dispatch() */
int boot_trace_armed;
#endif

/* 从核报到标志，按逻辑 cpu 号索引。每格只由对应从核写、引导核只读，不需要锁。 */
static volatile int secondary_up[CORE_NUMBER];
/* 等从核报到的自旋上限，宽到不可能误判，又不至于挂住太久。 */
#define SECONDARY_UP_SPIN_LIMIT 100000000UL

static void init_print_banner(void)
{
    /* 必须是二维字符数组：指针数组存的是链接时的高位虚拟地址，而此时 MMU 未开、按
     * 物理地址运行，一解引用就访问异常（trap 未初始化，表现为一个字都打不出来）。 */
    static const char banner[][64] = {
        " ____   ____   _                  ___   ____         /\\",
        "|  _ \\ / ___| | |_   __ _  _ __  / _ \\ / ___|   ____/  \\____",
        "| | | |\\___ \\ | __| / _` || '__|| | | |\\___ \\    \\        /",
        "| |_| | ___) || |_ | (_| || |   | |_| | ___) |   /   /\\   \\",
        "|____/ |____/  \\__| \\__,_||_|    \\___/ |____/   /___/  \\___\\",
    };

    for (uint32_t i = 0; i < sizeof(banner) / sizeof(banner[0]); i++)
    {
        printf("%s\n", banner[i]);
    }
#if defined(VF2)
    printf("\n     RISC-V rv64 kernel  -  VisionFive 2\n");
#else
    printf("\n     RISC-V rv64 kernel  -  QEMU virt\n");
#endif
}

/**
 * @brief 引导核开 MMU 之前的初始化：控制台、SBI、设备树、探测 hart、PMM 与内核页表
 */
void os_init_before_mmu_enable(void)
{
    console_init();
    init_print_banner();
    printf("DStarOS is starting...\n");
    sbi_init();
    printf("dtb: phys addr 0x%lx\n", dtb_phys_addr);
    /* 这三步必须排在 MMU 开启之前：一开 MMU，DTB 就不在内核偏移映射范围内了
     * （VF2 上它甚至在 KERNEL_MAP_END 之外）。 */
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

/* 经 SBI HSM 启动全部从核，HSM 不支持或启动失败则退回单核。
 * 有界等待：SBI 报 SUCCESS 不代表从核真的走到了高 VA。死等会整机停住、串口
 * 再无一个字；有界之后最坏也只是少几个核，还留下一条可见日志。 */
static void start_secondary_harts(void)
{
    int started = 0;
    for (int id = 1; id < cpu_get_present_count(); id++)
    {
        if (cpu_start_secondary_hart((uint16_t)id) == SBI_SUCCESS)
        {
            started += 1;
        }
    }

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

    /* 所有已请求启动的从核都过了 trampoline 才能拆恒等映射——超时那一路不能拆，
     * 姗姗来迟的从核一 csrw satp 就跑飞。留着只多占一段内核低半区 VA：用户页表
     * 只复制内核高半段，对 U 态不可见。 */
    if (up == started)
    {
        vmm_remove_identity_mapping();
    }
}

/**
 * @brief 各 hart 开 MMU、经 trampoline 跳到高地址之后的初始化
 * @param[in] cpu_id 逻辑 cpu 号（引导核恒为 0），不是 hartid
 */
void os_init_after_mmu_enable(uint64_t cpu_id)
{
    cpu_set_core_id(cpu_id);
    if (cpu_id == 0)
    {
#if DEBUG_BRINGUP
        /* 必须绕开 printf：它存的 stdout_putc 还是物理地址，此刻经函数指针调过去会 fault。
         * 只让引导核打——这里还没有 ConsoleLock，多核输出会交错。 */
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
        /* 必须排在 trap_init() 之后：映射不对时这里会缺页，而 stvec 未设时表现为
         * 串口完全静默，无从诊断。 */
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
        sched_init();   /* 全局就绪队列只能由引导核初始化一次，否则后起的从核会把 init 冲掉 */
        proc_early_init(); /* proc_list / proc_list_lock / pid_lock，必须早于启动从核 */

        /* 启动从核必须排在 proc_init() 之前：fork 出 init 时引导核会被抢占，剩下的
         * 启动代码要等 idle 重新被调度才继续。 */
        start_secondary_harts();

        printf("core %ld init done\n", cpu_get_core_id());

        proc_init();     /* hart 0 的 idle + init；放在最后，此后被抢占都无所谓 */
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
    }

    idle(); /* 当前执行上下文成为 idle 线程，永不返回 */
    sbi_shutdown(); /* unreachable */
}
