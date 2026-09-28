/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _TRAP_H
#define _TRAP_H

#include <stdint.h>
#include <stdbool.h>

#include "encoding.h"
#include "debug.h"

/* Machine interrupt mask for 64 bit system, 0x8000 0000 0000 0000 */
#define CAUSE_MACHINE_IRQ_MASK (0x1ULL << 63)

/* Machine interrupt reason mask for 64 bit system, 0x7FFF FFFF FFFF FFFF */
#define CAUSE_MACHINE_IRQ_REASON_MASK (CAUSE_MACHINE_IRQ_MASK - 1)

/* Hypervisor interrupt mask for 64 bit system, 0x8000 0000 0000 0000 */
#define CAUSE_HYPERVISOR_IRQ_MASK (0x1ULL << 63)

/* Hypervisor interrupt reason mask for 64 bit system, 0x7FFF FFFF FFFF FFFF */
#define CAUSE_HYPERVISOR_IRQ_REASON_MASK (CAUSE_HYPERVISOR_IRQ_MASK - 1)

/* Supervisor interrupt mask for 64 bit system, 0x8000 0000 0000 0000 */
#define CAUSE_SUPERVISOR_IRQ_MASK (0x1ULL << 63)

/* Supervisor interrupt reason mask for 64 bit system, 0x7FFF FFFF FFFF FFFF */
#define CAUSE_SUPERVISOR_IRQ_REASON_MASK (CAUSE_SUPERVISOR_IRQ_MASK - 1)

#define CAUSE_FAULT_INSTRUCTION_PAGE 0xc
#define CAUSE_FAULT_LOAD_PAGE 0xd
#define CAUSE_FAULT_STORE_PAGE 0xf

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
    uint64_t sbadaddr;
};
typedef struct int_stackframe intstkf_t;

/* cpua.S 的 trap_entry/trap_return 用字面量 35*REGBYTES 开这个帧、并用
 * `sscratch = sp + 35*REGBYTES` 反算内核栈顶。字段数一改就会与汇编脱节，
 * 而症状是"用户态从垃圾 sepc 开始执行"这种极难反推的东西，所以在这里钉死。 */
_Static_assert(sizeof(struct int_stackframe) == 35 * 8,
               "intstkf_t must stay 35 registers wide (cpua.S hardcodes 35*REGBYTES)");

/* 保存当前中断状态并关闭中断；do{}while(0)用于保证外部操作与宏之间不会相互影响。必须与"localIntrRestore"成对使用。 */
#define __local_intr_save(x) \
    do                     \
    {                      \
        x = __intr_save();  \
    } while (0)

/* 还原上一次的中断状态；必须与"localIntrSave"成对使用。 */
#define __local_intr_restore(x) __intr_restore(x)

void trap_init(void);
/* cpua.S：stvec 指向 trap 入口，sscratch 清零 */
void trap_init_asm(void);
/* 关闭当前CPU的中断。 */
void local_intr_disable(void);
/* 打开当前CPU的中断。 */
void local_intr_enable(void);
void trap_handler(intstkf_t *sp);

#if DEBUG_INTSTACK
void print_intstk(intstkf_t *sp);
#endif

extern void tick_int_handler(void);

/* 返回中断的打开/关闭状态；并且关闭中断（如果处于打开状态）。 */
static inline bool __intr_save(void)
{
    if (read_csr(sstatus) & SSTATUS_SIE)
    {
        local_intr_disable();
        return true;
    }
    return false;
}

static inline void __intr_restore(bool flag)
{
    if (flag)
    {
        local_intr_enable();
    }
}

#endif
