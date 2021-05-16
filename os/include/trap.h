#ifndef _TRAP_H
#define _TRAP_H

#include <stdint.h>

#include "encoding.h"
#include "tinyprintf.h"
#include "debug.h"

/* Machine interrupt mask for 64 bit system, 0x8000 0000 0000 0000 */
#define CAUSE_MACHINE_IRQ_MASK            (0x1ULL << 63)

/* Machine interrupt reason mask for 64 bit system, 0x7FFF FFFF FFFF FFFF */
#define CAUSE_MACHINE_IRQ_REASON_MASK     (CAUSE_MACHINE_IRQ_MASK - 1)

/* Hypervisor interrupt mask for 64 bit system, 0x8000 0000 0000 0000 */
#define CAUSE_HYPERVISOR_IRQ_MASK         (0x1ULL << 63)

/* Hypervisor interrupt reason mask for 64 bit system, 0x7FFF FFFF FFFF FFFF */
#define CAUSE_HYPERVISOR_IRQ_REASON_MASK  (CAUSE_HYPERVISOR_IRQ_MASK - 1)

/* Supervisor interrupt mask for 64 bit system, 0x8000 0000 0000 0000 */
#define CAUSE_SUPERVISOR_IRQ_MASK         (0x1ULL << 63)

/* Supervisor interrupt reason mask for 64 bit system, 0x7FFF FFFF FFFF FFFF */
#define CAUSE_SUPERVISOR_IRQ_REASON_MASK  (CAUSE_SUPERVISOR_IRQ_MASK - 1)

struct int_stackframe
{
    uint64_t scause;
    uint64_t x1_ra;
    uint64_t x2_sp;
    uint64_t x3_gp;
    uint64_t x4_tp;
    uint64_t x5_t0;
    uint64_t x6_t1;
    uint64_t x7_t2;
    uint64_t x8_s0;
    uint64_t x9_s1;
    uint64_t x10_a0;
    uint64_t x11_a1;
    uint64_t x12_a2;
    uint64_t x13_a3;
    uint64_t x14_a4;
    uint64_t x15_a5;
    uint64_t x16_a6;
    uint64_t x17_a7;
    uint64_t x18_s2;
    uint64_t x19_s3;
    uint64_t x20_s4;
    uint64_t x21_s5;
    uint64_t x22_s6;
    uint64_t x23_s7;
    uint64_t x24_s8;
    uint64_t x25_s9;
    uint64_t x26_s10;
    uint64_t x27_s11;
    uint64_t x28_t3;
    uint64_t x29_t4;
    uint64_t x30_t5;
    uint64_t x31_t6;
    uint64_t sepc;
    uint64_t sstatus;
};

typedef struct int_stackframe intstkf_t;

void trap_init(void);
void irq_disable(void);
void irq_enable(void);
void trap_handle(intstkf_t *sp);

#if DEBUG_INTSTACK
void print_intstk(intstkf_t *sp);
#endif

extern void tick_int_handler(void);

#endif