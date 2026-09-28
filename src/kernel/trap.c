/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "trap.h"
#include "memtype.h"
#include "console.h"
#include "sync.h"
#include "plic.h"
#include "tty.h"
#include "uart.h"
#include "sbi.h"
#include "vmm.h"
#include "syscall.h"
#include "signal.h"
#include "proc.h"

/**
 * @brief 处理一次由指令自身引发的同步异常：U 态发起的只杀该进程，S 态发起的 panic
 * @param[in] sp   本次 trap 的寄存器帧
 * @param[in] what 异常名，原样进诊断行
 * @param[in] sig  U 态时用来杀掉该进程的信号
 * @details sstatus.SPP 是进入本次 trap 之前的特权级，中间没有嵌套 trap，所以它就是
 *   "谁执行了这条指令"。stval（本项目里叫 sbadaddr）的含义随异常而变：非法指令时是
 *   出错指令的编码（规范允许硬件填 0），取指/访存类异常时是出错地址。
 * @note U 态路径不返回（走 do_exit_signal，父进程 wait 到的 status 低 7 位就是 sig）。
 */
static void trap_user_exception(intstkf_t *sp, const char *what, int sig)
{
    pcb_t *curr = proc_get_current();
    int from_kernel = (read_csr(sstatus) & SSTATUS_SPP) ? 1 : 0;
    printf("trap: %s sepc=0x%lx stval=0x%lx spp=%d pid=%d\n",
           what, sp->sepc, sp->sbadaddr, from_kernel, curr ? curr->proc_pid : -1);
    if (!from_kernel)
    {
        do_exit_signal(sig);
    }
    panic("%s in kernel mode", what);
}


/**
 * @brief S 态外部中断：从 PLIC 领取、分发、交还
 * @details 目前唯一路由过来的是 UART 接收中断，且只路由给 cpu0（见 plic_init()）。
 *   处理复用 tick 轮询的同一个 tty_poll_input()：它循环把接收 FIFO 取空，电平触发的
 *   UART 中断因此得以撤销。tick 轮询保留作兜底，两条路径共用 tty 锁。
 * @note 领到 0 表示中断已撤销（例如 tick 轮询先一步把 FIFO 取空），无需交还。
 */
static void trap_external_irq(void)
{
    uint32_t irq = plic_claim();
    if (irq == 0)
    {
        return;
    }
#if DEBUG_BOOT_TRACE
    static int bt_ext_irq_reports;
    if (bt_ext_irq_reports < 8)
    {
        bt_ext_irq_reports++;
        BOOT_TRACE("external irq claimed");
    }
#endif
#if DEBUG_EXT_IRQ
    static uint64_t ext_irq_count;
    ext_irq_count += 1;
    printf("extirq: irq %u on cpu %ld (#%ld)\n", irq, (long)cpu_get_core_id(), (long)ext_irq_count);
#endif
    if (irq == UART_IRQ)
    {
        uart_handle_irq();
        tty_poll_input();
    }
    else
    {
        printf("trap: unexpected external irq %u\n", irq);
    }
    plic_complete(irq);
}

/**
 * @brief 设置本 hart 的 trap 入口并打开中断
 */
void trap_init(void)
{
    trap_init_asm();
    set_csr(sstatus, SSTATUS_SIE);
    set_csr(sie, MIP_SSIP | MIP_STIP | MIP_SEIP);

    /* @todo SUM 全程开着，正确做法是只在 copy_to/from_user 前后开。 */
    set_csr(sstatus, SSTATUS_SUM);

    printf("core %ld trap inited!\n", cpu_get_core_id());
}

/**
 * @brief 关本 hart 的 S 态全局中断
 */
void local_intr_disable(void)
{
    clear_csr(sstatus, SSTATUS_SIE);
}

/**
 * @brief 开本 hart 的 S 态全局中断
 */
void local_intr_enable(void)
{
    set_csr(sstatus, SSTATUS_SIE);
}

static bool get_local_intr(void)
{
    return (read_csr(sstatus) & SSTATUS_SIE) != 0;
}

/**
 * @brief trap 的实际分发逻辑
 */
static void trap_dispatch(intstkf_t *sp)
{
    int cause = sp->scause & CAUSE_SUPERVISOR_IRQ_REASON_MASK;

    if (get_local_intr())
    {
        /* 硬件进 trap 时自动清 SIE；这里还是 1 说明有人在关中断区间外进了 trap。 */
        panic("%s::interrupts enabled.\n", __FUNCTION__);
    }
#if DEBUG_BOOT_TRACE
    static int bt_trap_reports;
    if (boot_trace_armed && bt_trap_reports < 16)
    {
        bt_trap_reports++;
        boot_trace_hex("trap scause", sp->scause);
    }
#endif

#if DEBUG_INTSTACK
    print_intstk(sp);
#endif

    if (sp->scause & (1UL << 63))
    {
        
        switch (cause)
        {
        case IRQ_S_SOFT:
            /* 只用来把 wfi 中的 hart 踢醒，不做调度决策。
             * 必须清本地 sip.SSIP，否则中断条件一直成立会立刻重新触发。 */
            clear_csr(sip, MIP_SSIP);
            break;
        case IRQ_S_TIMER:
            tick_int_handler();
            break;
        case IRQ_S_EXT:
            trap_external_irq();
            break;
        default:
#if DEBUG_INTSTACK
            print_intstk(sp);
#endif
            printf("Unknown interrupt\n");
            break;
        }
    }
    else
    {
        /* 只在追踪时打：syscall 走的是这条路径，每次都打会淹掉日志。 */
#if DEBUG_INTSTACK
        printf("\nException:\n");
#endif
        switch (cause)
        {
        case CAUSE_FAULT_LOAD:
        case CAUSE_FAULT_STORE:
            /* 访问异常（scause 5/7）不是缺页：目标地址后面没有设备或内存响应，缺页
             * 处理器补不出这种错。单独打一行，否则会被误读成"映射没建上"，修法相反。 */
            printf("%s access fault (no device or memory responds at this addr): "
                   "addr=0x%lx sepc=0x%lx\n",
                   (cause == CAUSE_FAULT_LOAD) ? "Load" : "Store",
                   (unsigned long)sp->sbadaddr, (unsigned long)sp->sepc);
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr,
                                   (cause == CAUSE_FAULT_LOAD) ? 1 : 2);
            return;
        case CAUSE_FAULT_INSTRUCTION_PAGE:
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr, 0);
            return;
        case CAUSE_FAULT_LOAD_PAGE:
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr, 1);
            return;
        case CAUSE_FAULT_STORE_PAGE:
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr, 2);
            return;
        case CAUSE_USER_ECALL:
            /* ecall 是 4 字节，sepc 指向它自己；不加 4 会 sret 回来重复执行。 */
            sp->sepc += 4;
            /* a0 马上会被返回值覆盖，而 SA_RESTART 重启该 syscall 时要原样还给它；
             * a7（调用号）在帧里原封不动，不用另存 */
            proc_get_current()->proc_syscall_orig_a0 = sp->x10_a0;
            sp->x10_a0 = (uint64_t)syscall_dispatch(sp);
            return;
        case CAUSE_SUPERVISOR_ECALL:
            printf("Environment call from S-mode");
            return;
        case CAUSE_MISALIGNED_FETCH:
            trap_user_exception(sp, "instruction address misaligned", SIGBUS);
            return;
        case CAUSE_FAULT_FETCH:
            trap_user_exception(sp, "instruction access fault", SIGSEGV);
            return;
        case CAUSE_ILLEGAL_INSTRUCTION:
            trap_user_exception(sp, "illegal instruction", SIGILL);
            return;
        case CAUSE_BREAKPOINT:
            trap_user_exception(sp, "breakpoint", SIGTRAP);
            return;
        case CAUSE_MISALIGNED_LOAD:
            trap_user_exception(sp, "load address misaligned", SIGBUS);
            return;
        case CAUSE_MISALIGNED_STORE:
            trap_user_exception(sp, "store address misaligned", SIGBUS);
            return;
        case CAUSE_HYPERVISOR_ECALL:
            printf("Environment call from H-mode");
            break;
        case CAUSE_MACHINE_ECALL:
            printf("Environment call from M-mode");
            break;
        default:
            printf("Unknown exception : %08x", cause);
            break;
        }
        panic("Not pageFaultHander or Ecall Exception!!");
    }
}

/**
 * @brief trap 总入口（由 cpua.S 的 trap_entry 调用）
 * @details 分发完之后，若本次 trap 来自 U 态就在这里投递挂起的信号：这是内核里唯一
 *   一个"手里有 trap 帧、下一步就是 sret"的位置，而信号投递要改 sepc/sp/a0。
 *   来自 S 态的 trap 跳过——那时 sret 回的是内核代码。
 */
void trap_handler(intstkf_t *sp)
{
    trap_dispatch(sp);

    if ((sp->sstatus & SSTATUS_SPP) == 0)
    {
        signal_handle_pending(sp);
    }
}

#if DEBUG_INTSTACK
/**
 * @brief 打印 trap 帧（调试用）
 */
void print_intstk(intstkf_t *sp)
{
    printf("\n=================================================================\n");
    printf("  ra       0x%08lx\n", sp->x1_ra);
    printf("  scause   0x%08lx\n", sp->scause);
    printf("  sp       0x%08lx\n", sp->x2_sp);
    printf("  tp       0x%08lx\n", sp->x4_tp);
    printf("  t0       0x%08lx\n", sp->x5_t0);
    printf("  t1       0x%08lx\n", sp->x6_t1);
    printf("  t2       0x%08lx\n", sp->x7_t2);
    printf("  s0       0x%08lx\n", sp->x8_s0);
    printf("  s1       0x%08lx\n", sp->x9_s1);
    printf("  a0       0x%08lx\n", sp->x10_a0);
    printf("  a1       0x%08lx\n", sp->x11_a1);
    printf("  a2       0x%08lx\n", sp->x12_a2);
    printf("  a3       0x%08lx\n", sp->x13_a3);
    printf("  a4       0x%08lx\n", sp->x14_a4);
    printf("  a5       0x%08lx\n", sp->x15_a5);
    printf("  a6       0x%08lx\n", sp->x16_a6);
    printf("  a7       0x%08lx\n", sp->x17_a7);
    printf("  s2       0x%08lx\n", sp->x18_s2);
    printf("  s3       0x%08lx\n", sp->x19_s3);
    printf("  s4       0x%08lx\n", sp->x20_s4);
    printf("  s5       0x%08lx\n", sp->x21_s5);
    printf("  s6       0x%08lx\n", sp->x22_s6);
    printf("  s7       0x%08lx\n", sp->x23_s7);
    printf("  s8       0x%08lx\n", sp->x24_s8);
    printf("  s9       0x%08lx\n", sp->x25_s9);
    printf("  s10      0x%08lx\n", sp->x26_s10);
    printf("  s11      0x%08lx\n", sp->x27_s11);
    printf("  t3       0x%08lx\n", sp->x28_t3);
    printf("  t4       0x%08lx\n", sp->x29_t4);
    printf("  t5       0x%08lx\n", sp->x30_t5);
    printf("  t6       0x%08lx\n", sp->x31_t6);
    printf("  sepc     0x%08lx\n", sp->sepc);
    printf("  sstatus  0x%08lx\n", sp->sstatus);
    printf("  sbadaddr 0x%08lx\n", sp->sbadaddr);
    printf("=================================================================\n");
}
#endif
