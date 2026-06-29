    # 与vmm.h SATPMODE_RV39 保持一致
    .equ SATPMODE_RV39, 0x8000000000000000
    
    .section .text.entry
    .globl _start
    .align 2
_start:
    bnez  a0, secondary_cpu_entry        # a0 restores core id because of sbi,bnez:不等于零时分支
    # s0是保存寄存器，调用函数（参考函数调用规范）os_init_before_mmu_enable后值不变，后续os_init_after_mmu_enable需要core id作为参数
    mv    s0, a0           
    la    sp, boot_stack_top1           # 相对寻址设置sp（低物理地址）

    # bss段清零（bss清0必须是叶函数，否则清除返回上层函数返回地址）
    # 等价于 *dst = 0 for (dst = edata; dst < ebss; dst += 8)
    la    t0, edata
    la    t1, ebss
    bgeu  t0, t1, bss_clear_done        # branch if greater or equal (unsigned)
bss_clear_loop:
    sd    zero, 0(t0)
    addi  t0, t0, 8
    bltu  t0, t1, bss_clear_loop        # branch if less than (unsigned)
bss_clear_done:

    call  os_init_before_mmu_enable
    li    t0, SATPMODE_RV39
    la    t1, vmm_kernel_pgd_ppn
    ld    t1, 0(t1)
    or    a0, t0, t1                    # 计算satp寄存器值存入a0：SATPMODE_RV39 | kernel_pgd_ppn
    la    t0, _trampoline_start         
    jr    t0

loop1:   j     loop1                    # enter the infinite loop

secondary_cpu_entry:
    la    t2, core2Enabled
    ld    t2, 0(t2)
    li    t3, 0xa55a
    beq   t2, t3, 1f
    j     secondary_cpu_entry

1:                                      # 1f
    mv    s0, a0
    la    sp, boot_stack_top2           # setup core2 stack
    li    t0, SATPMODE_RV39
    la    t1, vmm_kernel_pgd_ppn
    ld    t1, 0(t1)
    or    a0, t0, t1
    la    t0, _trampoline_start         
    jr    t0

loop2:   j     loop2                    # enter the infinite loop

    .data                               # 注意不是.section .data，后者会将此关键词以下的代码放到.data段中！
    .globl core2Enabled
    # .align N 表示对齐到 2^N 字节，N 由"后面的数据/代码将以多大的位宽被访问"决定
    .align 3 # 2^3=8字节对齐，.dword占8字节，所以对齐到8字节边界
core2Enabled:
    .dword 0

    .section .text.entry
    .globl getCoreId_asm
    .align 2 # 跳转/函数入口 这种取址访问要求对齐到4字节
getCoreId_asm:
    mv    a0, tp
    ret

    .globl setCoreId_asm
    .align 2
setCoreId_asm:
    mv    tp, a0
    ret

    # trampoline section，PA = VA（恒等映射区内）
    .section .text.trampoline
    .globl _trampoline_start
    .align 2
_trampoline_start:
    # a0 = SATPMODE_RV39 | kernel_pgd_ppn
    csrw  satp, a0                   # 使能 MMU
    sfence.vma zero, zero           # 刷新 TLB

    # 读出 _start_virtual 的高 VA 绝对值
    la    t0, _start_virtual_addr   # 恒等映射，相对寻址可达
    ld    t0, 0(t0)                 # 读出 .dword _start_virtual = 高 VA
    jr    t0                        # 跳转 PC ← 高 VA

    .align 3
_start_virtual_addr:
    .dword _start_virtual           

    .globl _trampoline_end
_trampoline_end:

    # 从_trampoline_start返回后，PC装载高VA，从这里继续
    .section .text
    .globl _start_virtual
    .align 2
_start_virtual:
    bnez  s0, 1f
    la    sp, boot_stack_top1           # 重新设置core0 sp（高虚拟地址）
    j     2f
1:  la    sp, boot_stack_top2           # 重新设置core1 sp（高虚拟地址）
2:  mv    a0, s0                        # 将core id传递给os_init_after_mmu_enable
    call  os_init_after_mmu_enable

    # 堆栈增长方向为由高地址向低地址增长
    .section .bss.stack
    .align 4  # 根据 RISC-V psABI（调用约定）规范：函数调用时 sp 中的地址值必须 16 字节对齐
    .globl boot_stack1
boot_stack1:
    .space 4096 * 8
    .globl boot_stack_top1
boot_stack_top1:

    .section .bss.stack
    .globl boot_stack2
    .align 4
boot_stack2:
    .space 4096 * 8
    .globl boot_stack_top2
boot_stack_top2: