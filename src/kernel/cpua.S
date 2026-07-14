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

    sd x1, 1*REGBYTES(sp)   # ra

    csrr  x1, scause
    sd x1, 0*REGBYTES(sp)

    csrrw x1, sscratch, zero    # x1 ← 触发时的原始 sp（用户sp 或 内核sp）；sscratch ← 0
    sd x1, 2*REGBYTES(sp)

    sd x3, 3*REGBYTES(sp)
    sd x4, 4*REGBYTES(sp)
    sd x5, 5*REGBYTES(sp)
    sd x6, 6*REGBYTES(sp)
    sd x7, 7*REGBYTES(sp)
    sd x8, 8*REGBYTES(sp)
    sd x9, 9*REGBYTES(sp)
    sd x10, 10*REGBYTES(sp)
    sd x11, 11*REGBYTES(sp)
    sd x12, 12*REGBYTES(sp)
    sd x13, 13*REGBYTES(sp)
    sd x14, 14*REGBYTES(sp)
    sd x15, 15*REGBYTES(sp)
    sd x16, 16*REGBYTES(sp)
    sd x17, 17*REGBYTES(sp)
    sd x18, 18*REGBYTES(sp)
    sd x19, 19*REGBYTES(sp)
    sd x20, 20*REGBYTES(sp)
    sd x21, 21*REGBYTES(sp)
    sd x22, 22*REGBYTES(sp)
    sd x23, 23*REGBYTES(sp)
    sd x24, 24*REGBYTES(sp)
    sd x25, 25*REGBYTES(sp)
    sd x26, 26*REGBYTES(sp)
    sd x27, 27*REGBYTES(sp)
    sd x28, 28*REGBYTES(sp)
    sd x29, 29*REGBYTES(sp)
    sd x30, 30*REGBYTES(sp)
    sd x31, 31*REGBYTES(sp)

    csrr  x1, sepc
    sd x1, 32*REGBYTES(sp)

    csrr  x1, sstatus
    sd x1, 33*REGBYTES(sp)

    csrr  x1, CSR_SBADADDR
    sd x1, 34*REGBYTES(sp)

    mv a0, sp

    call trap_handler

    .local trap_return
    .align 2
trap_return:
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
    ld x1, 32*REGBYTES(sp)
    csrw sepc, x1

    ld x1, 33*REGBYTES(sp)
    csrw sstatus, x1

    ld x1, 1*REGBYTES(sp)

    ld x3, 3*REGBYTES(sp)
    ld x4, 4*REGBYTES(sp)
    ld x5, 5*REGBYTES(sp)
    ld x6, 6*REGBYTES(sp)
    ld x7, 7*REGBYTES(sp)
    ld x8, 8*REGBYTES(sp)
    ld x9, 9*REGBYTES(sp)
    ld x10, 10*REGBYTES(sp)
    ld x11, 11*REGBYTES(sp)
    ld x12, 12*REGBYTES(sp)
    ld x13, 13*REGBYTES(sp)
    ld x14, 14*REGBYTES(sp)
    ld x15, 15*REGBYTES(sp)
    ld x16, 16*REGBYTES(sp)
    ld x17, 17*REGBYTES(sp)
    ld x18, 18*REGBYTES(sp)
    ld x19, 19*REGBYTES(sp)
    ld x20, 20*REGBYTES(sp)
    ld x21, 21*REGBYTES(sp)
    ld x22, 22*REGBYTES(sp)
    ld x23, 23*REGBYTES(sp)
    ld x24, 24*REGBYTES(sp)
    ld x25, 25*REGBYTES(sp)
    ld x26, 26*REGBYTES(sp)
    ld x27, 27*REGBYTES(sp)
    ld x28, 28*REGBYTES(sp)
    ld x29, 29*REGBYTES(sp)
    ld x30, 30*REGBYTES(sp)
    ld x31, 31*REGBYTES(sp)

    ld x2, 2*REGBYTES(sp)

    sret

    .section      .text
    .globl switch_to
    .align 2
switch_to:
    # save from's registers
    sd x1, 0*REGBYTES(x10)
    sd x2, 1*REGBYTES(x10)
    sd x8, 2*REGBYTES(x10)
    sd x9, 3*REGBYTES(x10)
    sd x18, 4*REGBYTES(x10)
    sd x19, 5*REGBYTES(x10)
    sd x20, 6*REGBYTES(x10)
    sd x21, 7*REGBYTES(x10)
    sd x22, 8*REGBYTES(x10)
    sd x23, 9*REGBYTES(x10)
    sd x24, 10*REGBYTES(x10)
    sd x25, 11*REGBYTES(x10)
    sd x26, 12*REGBYTES(x10)
    sd x27, 13*REGBYTES(x10)
    # 保存页表
    csrr t0, satp
    sd t0, 14*REGBYTES(x10)

    # restore to's registers
    ld x1, 0*REGBYTES(x11)
    ld x2, 1*REGBYTES(x11)
    ld x8, 2*REGBYTES(x11)
    ld x9, 3*REGBYTES(x11)
    ld x18, 4*REGBYTES(x11)
    ld x19, 5*REGBYTES(x11)
    ld x20, 6*REGBYTES(x11)
    ld x21, 7*REGBYTES(x11)
    ld x22, 8*REGBYTES(x11)
    ld x23, 9*REGBYTES(x11)
    ld x24, 10*REGBYTES(x11)
    ld x25, 11*REGBYTES(x11)
    ld x26, 12*REGBYTES(x11)
    ld x27, 13*REGBYTES(x11)
    # 切换页表
    ld t0, 14*REGBYTES(x11)
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