    .section .text.entry
    .globl _start
_start:
    /* setup stack */
    la    sp, boot_stack_top           # setup stack pointer
    call os_init
halt:   j     halt                    # enter the infinite loop

    .section .bss.stack
    .globl boot_stack
boot_stack:
    .space 4096 * 16
    .globl boot_stack_top
boot_stack_top: