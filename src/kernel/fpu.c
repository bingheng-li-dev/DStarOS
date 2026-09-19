/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "fpu.h"
#include "proc.h"
#include "encoding.h"

/* 浮点上下文的存取。
 *
 * 为什么是**无条件**存取而不是按 sstatus.FS 的 Dirty 位惰性保存：本内核的任务数是
 * 个位数、tick 是 200 Hz，33 条 load/store 的代价可以忽略；而惰性方案要在
 * trap 出入口维护 FS 状态机，多一处状态就多一类只在切换时机边界上出现的错。
 * 等 FP 真的成为热路径再谈优化。
 *
 * 内核自身不用浮点（kernel.elf 反汇编核对过零条 FP 指令），这两个函数是唯一的例外。
 */

#define FPU_SAVE_REG(n)  asm volatile("fsd f" #n ", %0" : "=m"(p->proc_fp_regs[n]))
#define FPU_LOAD_REG(n)  asm volatile("fld f" #n ", %0" : : "m"(p->proc_fp_regs[n]))

void fpu_init(void)
{
    /* Off -> Initial。清掉再置，不能直接 set_csr——FS 是两位域，
     * 直接 or 上去在 Dirty 状态下是恒等操作，看不出问题但也没做对。 */
    uint64_t s = read_csr(sstatus);
    s = (s & ~(uint64_t)SSTATUS_FS) | SSTATUS_FS_INITIAL;
    write_csr(sstatus, s);
}

void fpu_save(struct proc_control_block *p)
{
    FPU_SAVE_REG(0);  FPU_SAVE_REG(1);  FPU_SAVE_REG(2);  FPU_SAVE_REG(3);
    FPU_SAVE_REG(4);  FPU_SAVE_REG(5);  FPU_SAVE_REG(6);  FPU_SAVE_REG(7);
    FPU_SAVE_REG(8);  FPU_SAVE_REG(9);  FPU_SAVE_REG(10); FPU_SAVE_REG(11);
    FPU_SAVE_REG(12); FPU_SAVE_REG(13); FPU_SAVE_REG(14); FPU_SAVE_REG(15);
    FPU_SAVE_REG(16); FPU_SAVE_REG(17); FPU_SAVE_REG(18); FPU_SAVE_REG(19);
    FPU_SAVE_REG(20); FPU_SAVE_REG(21); FPU_SAVE_REG(22); FPU_SAVE_REG(23);
    FPU_SAVE_REG(24); FPU_SAVE_REG(25); FPU_SAVE_REG(26); FPU_SAVE_REG(27);
    FPU_SAVE_REG(28); FPU_SAVE_REG(29); FPU_SAVE_REG(30); FPU_SAVE_REG(31);
    asm volatile("frcsr %0" : "=r"(p->proc_fcsr));
}

void fpu_restore(struct proc_control_block *p)
{
    FPU_LOAD_REG(0);  FPU_LOAD_REG(1);  FPU_LOAD_REG(2);  FPU_LOAD_REG(3);
    FPU_LOAD_REG(4);  FPU_LOAD_REG(5);  FPU_LOAD_REG(6);  FPU_LOAD_REG(7);
    FPU_LOAD_REG(8);  FPU_LOAD_REG(9);  FPU_LOAD_REG(10); FPU_LOAD_REG(11);
    FPU_LOAD_REG(12); FPU_LOAD_REG(13); FPU_LOAD_REG(14); FPU_LOAD_REG(15);
    FPU_LOAD_REG(16); FPU_LOAD_REG(17); FPU_LOAD_REG(18); FPU_LOAD_REG(19);
    FPU_LOAD_REG(20); FPU_LOAD_REG(21); FPU_LOAD_REG(22); FPU_LOAD_REG(23);
    FPU_LOAD_REG(24); FPU_LOAD_REG(25); FPU_LOAD_REG(26); FPU_LOAD_REG(27);
    FPU_LOAD_REG(28); FPU_LOAD_REG(29); FPU_LOAD_REG(30); FPU_LOAD_REG(31);
    asm volatile("fscsr %0" : : "r"(p->proc_fcsr));
}
