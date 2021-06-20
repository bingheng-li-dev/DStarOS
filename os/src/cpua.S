#include "encoding.h"
#define REGBYTES 8

  .section      .text.entry
  .align 2
  .globl trap_init_asm
trap_init_asm:
  la t0, trap_entry
  csrw stvec, t0
  ret

  .globl trap_entry
  .align 2
trap_entry:
  csrw sscratch, sp

  addi  sp,  sp, -35 * REGBYTES

  sd x1, 1*REGBYTES(sp)

  csrr  x1, scause
  sd x1, 0*REGBYTES(sp)

  csrrw x1, sscratch, x0
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

  call trap_handle

  .globl trap_return
  .align 2
trap_return:
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