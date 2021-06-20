#include "trap.h"

void trap_init(void)
{
    trap_init_asm();
    printf("trap inited!\n");
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

void trap_handle(intstkf_t *sp)
{
    int cause = sp->scause & CAUSE_MACHINE_IRQ_REASON_MASK;

#if DEBUG_INTSTACK
    print_intstk(sp);
#endif

    if (sp->scause & (1UL << 63))
    {
        switch (cause)
        {
        case IRQ_S_SOFT:
            printf("Supervisor software interrupt\n");
            break;
        case IRQ_S_TIMER:
            // printf("Supervisor timer interrupt\n");
            tick_int_handler();
            break;
        case IRQ_S_EXT:
            printf("Supervisor external interrupt\n");
            break;
        default:
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
            break;
        case CAUSE_FAULT_FETCH:
            printf("Instruction access fault");
            break;
        case CAUSE_ILLEGAL_INSTRUCTION:
            printf("Illegal instruction");
            break;
        case CAUSE_BREAKPOINT:
            printf("Breakpoint");
            break;
        case CAUSE_MISALIGNED_LOAD:
            printf("Load address misaligned");
            break;
        case CAUSE_FAULT_LOAD:
            printf("Load access fault");
            vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        case CAUSE_MISALIGNED_STORE:
            printf("Store address misaligned");
            break;
        case CAUSE_FAULT_STORE:
            printf("Store access fault");
            vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        case CAUSE_USER_ECALL:
            printf("Environment call from U-mode");
            break;
        case CAUSE_SUPERVISOR_ECALL:
            printf("Environment call from S-mode");
            break;
        case CAUSE_HYPERVISOR_ECALL:
            printf("Environment call from H-mode");
            break;
        case CAUSE_MACHINE_ECALL:
            printf("Environment call from M-mode");
            break;
        case CAUSE_FAULT_INSTRUCTION_PAGE:
            printf("Instruction page fault");
            break;
        case CAUSE_FAULT_LOAD_PAGE:
            vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        case CAUSE_FAULT_STORE_PAGE:
            vmm_pageFaultHander((virAddr_t)sp->sbadaddr);
            break;
        default:
            printf("Unknown exception : %08x", cause);
            break;
        }
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
