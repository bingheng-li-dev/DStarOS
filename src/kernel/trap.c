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

extern void trap_init_asm(void);

/**
 * @brief 处理一次由指令自身引发的同步异常：U 态发起的只杀该进程，S 态发起的 panic
 * @param[in] sp   本次 trap 的寄存器帧
 * @param[in] what 异常名，原样进诊断行
 * @param[in] sig  U 态时用来杀掉该进程的信号
 * @details 判据与 vmm.c 的 vmm_segfault 同源——sstatus.SPP 记录的是进入本次 trap
 *   之前的特权级，从异常发生到这里之间没有嵌套 trap，所以它就是"谁执行了这条指令"。
 *   用户程序自己作死不该拖垮内核；内核执行到了才说明是内核 bug。
 *
 *   stval（本项目里叫 sbadaddr）的含义随异常而变：非法指令时是**出错指令自身的编码**
 *   （规范允许硬件填 0），取指/访存类异常时是**出错的地址**。两种都和 sepc 一并打出来：
 *   拿 sepc 去反汇编、拿 stval 看编码或地址，两边一对就能定位。
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

void trap_init(void)
{
    trap_init_asm();
    /* sstatus寄存器的sie位是中断全局使能。 */
    set_csr(sstatus, SSTATUS_SIE);
    /* 全局使能以后，在sie寄存器中分别使能软件中断，时钟中断，外部中断；S态外部中断需要rustSBI的支持。 */
    set_csr(sie, MIP_SSIP | MIP_STIP | MIP_SEIP);

    // @todo 暂时的，内核可全程访问U态页。正常应仅在copy_to/from_user函数前后使用
    set_csr(sstatus, SSTATUS_SUM);

    printf("core %ld trap inited!\n", cpu_get_core_id());
}

void local_intr_disable(void)
{
    /* sstatus寄存器的sie位是中断全局使能。 */
    clear_csr(sstatus, SSTATUS_SIE);
}

void local_intr_enable(void)
{
    /* sstatus寄存器的sie位是中断全局使能。 */
    set_csr(sstatus, SSTATUS_SIE);
#if DEBUG_LOCK_irq_enable
    /* 不能用 printf：printf 经过 ConsoleLock→spinlockRelease→localIntrEnable，
     * 会无限递归直到栈溢出。改用绕过锁的原始 SBI 输出。 */
    const char *msg = "local_intr_enable::irq enabled!!\n";
    for (const char *p = msg; *p; p++)
        sbi_console_putchar((int)*p);
#endif
}

/* 返回当前core(local core)的全局中断是否处于使能状态。 */
static bool get_local_intr(void)
{
    return (read_csr(sstatus) & SSTATUS_SIE) != 0;
}

/**
 * @brief trap 的实际分发逻辑
 * @details 从 trap_handler() 里拆出来，只是为了给"返回 U 态之前投递信号"找一个
 *   兜得住的位置：本函数里散布着多条 return（缺页、ecall 各一条），在每条 return
 *   前面各加一次检查迟早会漏，包一层最省事。
 */
static void trap_dispatch(intstkf_t *sp)
{
    int cause = sp->scause & CAUSE_SUPERVISOR_IRQ_REASON_MASK;

    if (get_local_intr())
    {
        /* 发生中断后，硬件会自动将SSTATUS_SIE位置0。如果不是0说明出错了。 */
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
            /* 核间中断：由 sched_activate() 在新任务入队后经 cpu_send_ipi() 发出，
             * 只用来把 wfi 中的 hart 踢醒，让它回到 idle() 循环重新检查就绪队列，
             * 不需要在这里做任何调度决策；
             * 必须清本地 sip.SSIP，否则中断条件一直成立会立刻重新触发。 */
            clear_csr(sip, MIP_SSIP);
            break;
        case IRQ_S_TIMER:
            // printf("Supervisor timer interrupt\n");s
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
        /* 只在追踪 trap 时才打这条 banner：正常处理掉的异常（用户 ecall、懒分配/COW
         * 缺页）会走到下面的 return，每个 syscall 都打一条会把日志淹掉，还在 syscall
         * 热路径上白白吃一次 ConsoleLock + SBI 调用；而真正意外的异常在各自的 case 里
         * 都会先打印具体原因再 panic（panic 自带位置信息），并不依赖这条 banner。 */
#if DEBUG_INTSTACK
        printf("\nException:\n");
#endif
        switch (cause)
        {
        case CAUSE_FAULT_LOAD:
        case CAUSE_FAULT_STORE:
            /* 访问异常（scause 5/7）**不是缺页**：目标地址后面根本没有设备或内存响应，
             * 缺页处理器补不出这种错。单独打一行点明，否则它会一路走到
             * "kernel page fault" 那条 panic，把"地址后面没东西"误读成"映射没建上"
             * ——两者的修法完全相反。MMIO / 设备驱动调试期这是常见故障形态。 */
            printf("%s access fault (no device or memory responds at this addr): "
                   "addr=0x%lx sepc=0x%lx\n",
                   (cause == CAUSE_FAULT_LOAD) ? "Load" : "Store",
                   (unsigned long)sp->sbadaddr, (unsigned long)sp->sepc);
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr,
                                   (cause == CAUSE_FAULT_LOAD) ? 1 : 2);
            return;
        case CAUSE_FAULT_INSTRUCTION_PAGE:
#if DEBUG_VMM_page_fault_handler
            printf("Instruction page fault\n");
#endif
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr, 0);
            return;
        case CAUSE_FAULT_LOAD_PAGE:
#if DEBUG_VMM_page_fault_handler
            printf("Load page fault\n");
            printf("sbadaddr=0x%lx\n", sp->sbadaddr);
#endif
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr, 1);
            return;
        case CAUSE_FAULT_STORE_PAGE:
#if DEBUG_VMM_page_fault_handler
            printf("Store page fault\n");
            printf("sbadaddr=0x%lx\n", sp->sbadaddr);
#endif
            vmm_page_fault_handler((virAddr_t)sp->sbadaddr, 2);
            return;
        case CAUSE_USER_ECALL:
            /**
             * ecall 指令本身是 4 字节，
             * 硬件触发 trap 时 sepc 保存的是 触发 ecall 那条指令的 PC，即指向 ecall 自身；
             * sret 返回时 PC ← sepc，如果不加 4，返回后会再次执行 ecall，无限重入
             */
            sp->sepc += 4;
            /* a0 马上会被返回值覆盖，而 SA_RESTART 重启该 syscall 时要原样还给它；
             * a7（调用号）在帧里原封不动，不用另存 */
            proc_get_current()->proc_syscall_orig_a0 = sp->x10_a0;
            sp->x10_a0 = (uint64_t)syscall_dispatch(sp);
            return;
        case CAUSE_SUPERVISOR_ECALL:
            printf("Environment call from S-mode");
            return;
        /* 下面这六条形状相同：都是"某条指令自己作死"，U 态触发只杀该进程。
         * 信号映射按 POSIX。此前只有非法指令一条这么做，其余五条一律 panic 整个内核
         * ——那意味着一个用户程序里的野指针取指就能带走整个操作系统。 */
        case CAUSE_MISALIGNED_FETCH:
            trap_user_exception(sp, "instruction address misaligned", SIGBUS);
            return;
        case CAUSE_FAULT_FETCH:
            trap_user_exception(sp, "instruction access fault", SIGSEGV);
            return;
        case CAUSE_ILLEGAL_INSTRUCTION:
            /* 用户态最常见的来源是浮点指令：本内核不保存 FP 上下文，构造进 U 态的
             * trapframe 时一律把 sstatus.FS 关死，于是任何 F/D 指令都会落到这里。
             * 这是**探针**——与其让浮点在没有上下文保存的情况下静默算错（切一次进程
             * 结果就变），不如让它当场响，把"用户程序到底用不用浮点"变成实测。 */
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
 * @details 分发完之后，若这次 trap 来自 U 态（sstatus.SPP == 0，即马上就要 sret
 *   回用户程序），就在这里投递挂起的信号——这是整个内核里唯一一个"手里有 trap 帧
 *   且下一步就是 sret"的位置，信号投递要改 sepc/sp/a0，只能在这里做。
 *   来自 S 态的 trap（内核自己缺页、时钟中断打断内核代码）一律跳过：那时 sret
 *   回的是内核代码，往用户栈上压帧没有意义。
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
