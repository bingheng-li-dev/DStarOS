    .section .text.entry
    .globl _start
_start:
    bnez a0, secondary_cpu_entry        # a0 restores core id because of sbi,bnez:不等于零时分支
    /* setup stack */
    la    sp, boot_stack_top1           # setup stack pointer
    call osInit
loop1:   j     loop1                    # enter the infinite loop

secondary_cpu_entry:
    la t2, core2Enabled
    ld t2, 0(t2)
    li t3, 0xa55a
    beq t2, t3, 1f
    j secondary_cpu_entry

1:                                      # 1f
    la    sp, boot_stack_top2           # setup core2 stack
    call osInit
loop2:   j     loop2                    # enter the infinite loop

    .data                               # 注意不是.section .data，后者会将此关键词以下的代码放到.data段中！
    .globl core2Enabled
    .align 3
core2Enabled:
    .dword 0

    .globl getCoreId_asm
    .align 2
getCoreId_asm:
    mv a0, tp
    ret

    .globl setCoreId_asm
    .align 2
setCoreId_asm:
    mv tp, a0
    ret

    # 堆栈增长方向为由高地址向低地址增长
    .section .bss.stack
    .globl boot_stack1
boot_stack1:
    .space 4096 * 8
    .globl boot_stack_top1
boot_stack_top1:

    .section .bss.stack
    .globl boot_stack2
boot_stack2:
    .space 4096 * 8
    .globl boot_stack_top2
boot_stack_top2: