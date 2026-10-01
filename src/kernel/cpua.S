/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

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
    # U 态触发 trap 时 sp 是用户栈，不能在上面建帧，要换到内核栈。约定：
    #   运行在 U 态时，sscratch 持有该进程的内核栈顶；
    #   运行在 S 态时，sscratch 为 0。
    csrrw sp, sscratch, sp  # 交换：来自 U 则 sp←内核栈顶, sscratch←用户sp；来自 S 则 sp←0, sscratch←内核sp
    bnez sp, 1f             # sp != 0 → 来自 U 态（sscratch 原本存内核栈顶）
    csrr  sp, sscratch      # 来自 S 态：sscratch 现含触发时的内核 sp，装回 sp（不再交换回去）
    addi  sp,  sp, -35 * REGBYTES
    sd    tp, 4*REGBYTES(sp)    # 来自 S 态：tp 本来就是 hart 号，原样存下（返回 S 态时不恢复）
    j 2f

1:
    # 来自 U 态：tp 现在是用户的 TLS 指针（用户可直接写 tp，不经 syscall），必须换回
    # 本 hart 的编号——内核拿 tp 当 hart 号（cpu_get_current() = &cpus[tp]）。
    # 次序要紧：先把用户的 tp 存进帧、再覆盖它，否则返回 U 态时还给用户的是 hart 号。
    addi  sp,  sp, -35 * REGBYTES
    sd    tp, 4*REGBYTES(sp)    # 先存用户的 tp，返回 U 态时要原样还给它
    # 再从内核栈顶的保留槽取回本 hart 号（见 proc.h 的 KSTACK_RESERVED）。
    # 该槽由 trap_return 在上一次返回 U 态之前写好；压帧后栈顶在 35*REGBYTES(sp) 处。
    ld    tp, 35*REGBYTES(sp)

2:

    sd ra, 1*REGBYTES(sp)

    csrr  ra, scause
    sd ra, 0*REGBYTES(sp)

    csrrw ra, sscratch, zero    # ra ← 触发时的原始 sp（用户sp 或 内核sp）；sscratch ← 0
    sd ra, 2*REGBYTES(sp)

    sd gp, 3*REGBYTES(sp)
    # tp 已在上面按 U/S 两条路径分别存过，这里不重复
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
    # 内核默认不能访问用户页，SUM 只在 user_access_begin/end 之间置位；sret 前随帧恢复
    li ra, SSTATUS_SUM
    csrc sstatus, ra

    csrr  ra, CSR_SBADADDR
    sd ra, 34*REGBYTES(sp)

    mv a0, sp

    call trap_handler

    .local trap_return
    .align 2
trap_return:
    # 检查是否需要发生调度
    call sched_preempt_if_needed
    # 从这里到 sret 必须关中断：下面要把 sscratch 置成非零，而 trap_entry 靠它判断来自 U 态，
    # 这段里被中断打进来会被误判，新帧压在正要 sret 的帧上。fork_out_asm 跳进来的路径
    # （新进程、enter_user_mode）是开着中断到这里的；sret 会用 SPIE 恢复中断。
    csrci sstatus, SSTATUS_SIE
    # 在恢复寄存器之前，依据将要恢复的 sstatus.SPP 设置 sscratch
    ld t0, 33*REGBYTES(sp)      # 取将恢复的 sstatus，SPP 记录进入中断之前处理器为U还是S
    andi t1, t0, SSTATUS_SPP
    bnez t1, 1f                 # 0为U态，1为S态
    addi t2, sp, 35*REGBYTES    # 返回 U 态：sscratch ← 内核栈顶
    sd tp, 0(t2)                # 把本 hart 号存进栈顶保留槽，供下次 trap_entry 取回
    csrw sscratch, t2
    # 恢复用户自己的 tp（TLS 指针）。必须排在上面那条 sd 之后：那条用的是内核的 tp。
    # 从这里到 sret 之间没有任何代码读 tp，而且中断已经关了（上面的 csrci），
    # 所以带着用户的 tp 走完这段尾巴是安全的。
    ld tp, 4*REGBYTES(sp)
    j 2f

1:
    csrw sscratch, zero         # 返回 S 态：sscratch ← 0
                                # 不恢复 tp：S 态继续用内核的 hart 号，理由见下方注释

2:
    ld ra, 32*REGBYTES(sp)
    csrw sepc, ra

    ld ra, 33*REGBYTES(sp)
    csrw sstatus, ra

    ld ra, 1*REGBYTES(sp)

    ld gp, 3*REGBYTES(sp)
    # 这里不恢复 tp：返回 U 态的路已在上面单独恢复，到这里的只剩返回 S 态。
    # tp 在 S 态是 hart 号，在 U 态是用户 TLS 指针，帧里那份只对返回 U 态有意义；对 S 态它
    # 是陈旧的——① do_fork 复制的父进程帧带着父进程所在的 hart 号；② 系统调用中途被换出、
    # 由另一个 hart 换回时帧里仍是旧 hart 号。恢复它会让 proc_get_current() 取到别的 hart
    # 的当前任务。switch_to 同理不保存/恢复 tp。
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
    # s0 = 线程函数，s1 = 参数。函数返回后跳 do_exit，这里不能用 call。
    mv a0, s1
    jalr s0
    jal  do_exit