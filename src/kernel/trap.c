#include "trap.h"
#include "memtype.h"
#include "console.h"
#include "cpu.h"
#include "plic.h"
#include "sbi.h"

extern void trapInit_asm(void);

static void kernelExternIrqHandler(void)
{
    /* 如果确实是有外部中断。理论上这个if语句可以去掉。 */
    if (read_csr(sip) & MIP_SEIP)
    {
        uint64_t irq = plicClaim();
        if (UART_IRQ == irq)
        {
            int c = sbi_console_getchar();
            if (-1 != c)
            {
                //Sth to do on console...
                printf("%c\n", c);
            }
        }
        else if (DISK_IRQ == irq)
        {
            //disk_intr();
        }
        else if (irq)
        {
            printf("unexpected interrupt irq = %d\n", irq);
        }
        if (irq)
        {
            plicComplete(irq);
        }
#ifndef QEMU
        /* clear pending bit. */
        set_csr(sip, read_csr(sip) & ~0x2);
        sbi_set_mie();
#endif
    }
}

void trapInit(void)
{
    trapInit_asm();
    /* sstatus寄存器的sie位是中断全局使能。 */
    set_csr(sstatus, SSTATUS_SIE);
    /* 全局使能以后，在sie寄存器中分别使能软件中断，时钟中断，外部中断；S态外部中断需要rustSBI的支持。 */
    set_csr(sie, MIP_SSIP | MIP_STIP | MIP_SEIP);
    printf("core %ld trap inited!\n", getCoreId());
}

void irq_disable(void)
{
    clear_csr(sstatus, SSTATUS_SIE);
}

void irq_enable(void)
{
    set_csr(sstatus, SSTATUS_SIE);
#if DEBUG_LOCK_irq_enable
    printf("irq_enable::irq enabled!!\n");
#endif
}

/* 返回当前core(local core)的全局中断是否处于使能状态。 */
bool getLocoreIntr(void)
{
    return (read_csr(sstatus) & SSTATUS_SIE) != 0;
}

void kernelTrapHandler(intstkf_t *sp)
{
    int cause = sp->scause & CAUSE_SUPERVISOR_IRQ_REASON_MASK;

    /* 发生异常之前的权限模式保留在 sstatus 的 SPP 域中。 */
    if ((read_csr(sstatus) & SSTATUS_SPP) == 0)
        panic("kernelTrapHandler::not from supervisor mode.");
    if (getLocoreIntr())
        /* 发生中断后，硬件会自动将SSTATUS_SIE位置0。如果不是0说明出错了。 */
        panic("kernelTrapHandler::interrupts enabled.");

#if DEBUG_INTSTACK
    // print_intstk(sp);
#endif

    if (sp->scause & (1UL << 63))
    {
        
        switch (cause)
        {
        case IRQ_S_SOFT:
            printf("Supervisor software interrupt\n");
            break;
        case IRQ_S_TIMER:
            // printf("Supervisor timer interrupt\n");s
            tickIntHandler();
            break;
        // case IRQ_S_EXT:
        //     printf("Supervisor external interrupt\n");
        //     kernelExternIrqHandler();
        // break;
        default:
            print_intstk(sp);
            if (0x8000000000000001L == sp->scause && 9 == read_csr(stval))
            {
                printf("Supervisor external interrupt\n");
                kernelExternIrqHandler();
            }
            else
                printf("Unknown interrupt\n");
            break;
        }
    }
    else
    {
        printf("\nException:\n");
        switch (cause)
        {
        case CAUSE_MISALIGNED_FETCH:
            printf("Instruction address misaligned");
            goto panic1;
            break;
        case CAUSE_FAULT_FETCH:
            printf("Instruction access fault");
            goto panic1;
            break;
        case CAUSE_ILLEGAL_INSTRUCTION:
            printf("Illegal instruction");
            goto panic1;
            break;
        case CAUSE_BREAKPOINT:
            printf("Breakpoint");
            goto panic1;
            break;
        case CAUSE_MISALIGNED_LOAD:
            printf("Load address misaligned");
            goto panic1;
            break;
        case CAUSE_FAULT_LOAD:
            printf("Load access fault");
            // vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        case CAUSE_MISALIGNED_STORE:
            printf("Store address misaligned");
            goto panic1;
            break;
        case CAUSE_FAULT_STORE:
            printf("Store access fault");
            // vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        case CAUSE_USER_ECALL:
            printf("Environment call from U-mode");
            break;
        case CAUSE_SUPERVISOR_ECALL:
            printf("Environment call from S-mode");
            break;
        case CAUSE_HYPERVISOR_ECALL:
            printf("Environment call from H-mode");
            goto panic1;
            break;
        case CAUSE_MACHINE_ECALL:
            printf("Environment call from M-mode");
            goto panic1;
            break;
        case CAUSE_FAULT_INSTRUCTION_PAGE:
            printf("Instruction page fault");
            goto panic1;
            break;
        case CAUSE_FAULT_LOAD_PAGE:
            // vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        case CAUSE_FAULT_STORE_PAGE:
            // vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        default:
            printf("Unknown exception : %08x", cause);
            break;
        }
    panic1:
        panic("Not pageFaultHander or Ecall Exception!!");
    }
}

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