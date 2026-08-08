#include "encoding.h"
#define REGBYTES 8

    .section      .text
    .align 2
    .globl trap_init_asm
trap_init_asm:
    la t0, trap_entry
    csrw stvec, t0
    csrw sscratch, zero # sscratch初始化为0，原因见trap_entry
    ret

    .local trap_entry
    .align 2
trap_entry:
    /** 
     * U 态触发 trap 时 sp 是用户栈，但不应在用户栈上建立中断内核栈帧，需要去内核栈
     * 业界标准约定，触发中断时：
     * 运行在 U 态时， sscratch 正持有该进程的内核栈顶；
     * 运行在 S 态时， sscratch 正持有0。
     */
    csrrw sp, sscratch, sp  # 交换：来自 U 则 sp←内核栈顶, sscratch←用户sp；来自 S 则 sp←0, sscratch←内核sp
    bnez sp, 1f             # sp != 0 → 来自 U 态（sscratch 原本存内核栈顶）
    csrr  sp, sscratch      # 来自 S 态：sscratch 现含触发时的内核 sp，装回 sp（不再交换回去）

1:
    addi  sp,  sp, -35 * REGBYTES

    sd ra, 1*REGBYTES(sp)

    csrr  ra, scause
    sd ra, 0*REGBYTES(sp)

    csrrw ra, sscratch, zero    # ra ← 触发时的原始 sp（用户sp 或 内核sp）；sscratch ← 0
    sd ra, 2*REGBYTES(sp)

    sd gp, 3*REGBYTES(sp)
    sd tp, 4*REGBYTES(sp)
    sd t0, 5*REGBYTES(sp)
    sd t1, 6*REGBYTES(sp)
    sd t2, 7*REGBYTES(sp)
    sd s0, 8*REGBYTES(sp)
    sd s1, 9*REGBYTES(sp)
    sd a0, 10*REGBYTES(sp)
    sd a1, 11*REGBYTES(sp)
    sd a2, 12*REGBYTES(sp)
    sd a3, 13*REGBYTES(sp)
    sd a4, 14*REGBYTES(sp)
    sd a5, 15*REGBYTES(sp)
    sd a6, 16*REGBYTES(sp)
    sd a7, 17*REGBYTES(sp)
    sd s2, 18*REGBYTES(sp)
    sd s3, 19*REGBYTES(sp)
    sd s4, 20*REGBYTES(sp)
    sd s5, 21*REGBYTES(sp)
    sd s6, 22*REGBYTES(sp)
    sd s7, 23*REGBYTES(sp)
    sd s8, 24*REGBYTES(sp)
    sd s9, 25*REGBYTES(sp)
    sd s10, 26*REGBYTES(sp)
    sd s11, 27*REGBYTES(sp)
    sd t3, 28*REGBYTES(sp)
    sd t4, 29*REGBYTES(sp)
    sd t5, 30*REGBYTES(sp)
    sd t6, 31*REGBYTES(sp)

    csrr  ra, sepc
    sd ra, 32*REGBYTES(sp)

    csrr  ra, sstatus
    sd ra, 33*REGBYTES(sp)

    csrr  ra, CSR_SBADADDR
    sd ra, 34*REGBYTES(sp)

    mv a0, sp

    call trap_handler

    .local trap_return
    .align 2
trap_return:
    # 检查是否需要发生调度
    call sched_preempt_if_needed
    # 在恢复寄存器之前，依据将要恢复的 sstatus.SPP 设置 sscratch
    ld t0, 33*REGBYTES(sp)      # 取将恢复的 sstatus，SPP 记录进入中断之前处理器为U还是S
    andi t1, t0, SSTATUS_SPP    # t1 = t0 & SSTATUS_SPP（0x0100），按位与后t1=0x100或0x000
    bnez t1, 1f                 # 0为U态，1为S态
    addi t2, sp, 35*REGBYTES    # 返回 U 态：sscratch ← 内核栈顶
    csrw sscratch, t2
    j 2f
    
1:
    csrw sscratch, zero         # 返回 S 态：sscratch ← 0

2:
    ld ra, 32*REGBYTES(sp)
    csrw sepc, ra

    ld ra, 33*REGBYTES(sp)
    csrw sstatus, ra

    ld ra, 1*REGBYTES(sp)

    ld gp, 3*REGBYTES(sp)
    ld tp, 4*REGBYTES(sp)
    ld t0, 5*REGBYTES(sp)
    ld t1, 6*REGBYTES(sp)
    ld t2, 7*REGBYTES(sp)
    ld s0, 8*REGBYTES(sp)
    ld s1, 9*REGBYTES(sp)
    ld a0, 10*REGBYTES(sp)
    ld a1, 11*REGBYTES(sp)
    ld a2, 12*REGBYTES(sp)
    ld a3, 13*REGBYTES(sp)
    ld a4, 14*REGBYTES(sp)
    ld a5, 15*REGBYTES(sp)
    ld a6, 16*REGBYTES(sp)
    ld a7, 17*REGBYTES(sp)
    ld s2, 18*REGBYTES(sp)
    ld s3, 19*REGBYTES(sp)
    ld s4, 20*REGBYTES(sp)
    ld s5, 21*REGBYTES(sp)
    ld s6, 22*REGBYTES(sp)
    ld s7, 23*REGBYTES(sp)
    ld s8, 24*REGBYTES(sp)
    ld s9, 25*REGBYTES(sp)
    ld s10, 26*REGBYTES(sp)
    ld s11, 27*REGBYTES(sp)
    ld t3, 28*REGBYTES(sp)
    ld t4, 29*REGBYTES(sp)
    ld t5, 30*REGBYTES(sp)
    ld t6, 31*REGBYTES(sp)

    ld sp, 2*REGBYTES(sp)

    sret

    .section      .text
    .globl switch_to
    .align 2
switch_to:
    # save from's registers
    sd ra, 0*REGBYTES(a0)
    sd sp, 1*REGBYTES(a0)
    sd s0, 2*REGBYTES(a0)
    sd s1, 3*REGBYTES(a0)
    sd s2, 4*REGBYTES(a0)
    sd s3, 5*REGBYTES(a0)
    sd s4, 6*REGBYTES(a0)
    sd s5, 7*REGBYTES(a0)
    sd s6, 8*REGBYTES(a0)
    sd s7, 9*REGBYTES(a0)
    sd s8, 10*REGBYTES(a0)
    sd s9, 11*REGBYTES(a0)
    sd s10, 12*REGBYTES(a0)
    sd s11, 13*REGBYTES(a0)
    # 保存页表
    csrr t0, satp
    sd t0, 14*REGBYTES(a0)

    # restore to's registers
    ld ra, 0*REGBYTES(a1)
    ld sp, 1*REGBYTES(a1)
    ld s0, 2*REGBYTES(a1)
    ld s1, 3*REGBYTES(a1)
    ld s2, 4*REGBYTES(a1)
    ld s3, 5*REGBYTES(a1)
    ld s4, 6*REGBYTES(a1)
    ld s5, 7*REGBYTES(a1)
    ld s6, 8*REGBYTES(a1)
    ld s7, 9*REGBYTES(a1)
    ld s8, 10*REGBYTES(a1)
    ld s9, 11*REGBYTES(a1)
    ld s10, 12*REGBYTES(a1)
    ld s11, 13*REGBYTES(a1)
    # 切换页表
    ld t0, 14*REGBYTES(a1)
    csrw satp, t0
    sfence.vma zero, zero

    ret

    .globl fork_out_asm
    .align 2
fork_out_asm:
    mv sp, a0
    j  trap_return
    
    .globl kernel_thread_entry
    .align 2
kernel_thread_entry:
    # "s1" restores the args,so move it to a0 as param.
    mv a0, s1
    # "s0" restores the address of the function,so jump to it.
    jalr s0
    # When the process ran over,jump to "do_exit",DO NOT USE "call" HERE!
    jal  do_exit