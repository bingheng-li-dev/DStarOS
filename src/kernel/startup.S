/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "platform.h"

#if defined(VF2)
    # RISC-V Linux Image 头（64 字节）。U-Boot 的 booti 靠它决定把内核搬到哪、搬多少，
    # 搬完**直接跳到镜像起点**——也就是下面 code0 那条指令。字段顺序与
    # Linux Documentation/riscv/boot-image-header.rst 一致，一个都不能挪。
    #
    # QEMU 侧不需要它（`-kernel` 直接按 ELF 的入口装载），所以只在 VF2 编进去；
    # 加了头之后 `_start` 不再是镜像第 0 字节，但 cpu_start_secondary_hart() 传的是
    # kva_to_pa(_start)，走的是链接符号而不是镜像偏移，不受影响。
    .section .image_header, "a", @progbits
    .globl _image_header
_image_header:
    # code0：booti 跳进来执行的第一条指令，**必须正好 4 字节**——若被压缩成 2 字节的
    # c.j，后面每个字段都会错位。
    # 实测当前配置下压不了（`j _start` 跨 section，汇编期算不出距离只能发完整 JAL；
    # 且 CFLAGS 里有 -mno-relax，链接期也不会缩），norvc 是**冗余保险**而不是唯一防线。
    # 留着它是因为这两个前提都可能变：把头挪进 .text.entry、或哪天去掉 -mno-relax，
    # 压缩就会真的发生，而症状是 U-Boot 报 magic 错、看不出根因在这里。
    .option push
    .option norvc
    j       _start
    .option pop
    .word   0                       # code1
    .quad   0x200000                # text_offset：相对 RAM 基址的偏移，
                                    # 必须与 memtype.h 的 KERNEL_START 低位一致
    .quad   ekernel - skernel       # image_size：含 .bss，即内核要占用的内存总量
    .quad   0                       # flags：bit0 = 0 表示小端
    .word   2                       # version：major 0 / minor 2
    .word   0                       # res1
    .quad   0                       # res2
    .quad   0x5643534952            # magic  "RISCV\0\0\0"
    .word   0x05435352              # magic2 "RSC\x05"
    .word   0                       # res3（PE/COFF 偏移，我们不用）
#endif

    # 与vmm.h SATPMODE_RV39 保持一致
    .equ SATPMODE_RV39, 0x8000000000000000

    .section .text.entry
    .globl _start
    # .align N 表示对齐到 2^N 字节，N 由"后面的数据/代码将以多大的位宽被访问"决定
    .align 2
_start:
    # 引导核判定：**第一个进来的就是引导核**，与 hartid 取值无关。
    # 不能再写成 `bnez a0, secondary`（a0 是 hartid）——那等价于假设"引导核一定是
    # hart 0"。QEMU virt 上碰巧成立，但换个平台就不一定：JH7110 的 hart 0 是没有 MMU、
    # 不支持 S 态的 S7 监控核，固件会屏蔽它，引导核是某个 U74。
    # 标志放 .data 而不是 .bss——下面的 bss 清零会把 .bss 抹平，那样后来的从核
    # 会重新抢到标志、把自己也当成引导核。
    la    t0, boot_hart_claimed
    li    t1, 1
    amoswap.d t2, t1, (t0)
    bnez  t2, secondary_cpu_entry

    # s0 存**逻辑 cpu 号**（不是 hartid），引导核恒为 0。它是保存寄存器，
    # 跨 os_init_before_mmu_enable 不变，后面 os_init_after_mmu_enable 要用。
    li    s0, 0
    # a1 是固件（OpenSBI/RustSBI 或 U-Boot booti）传进来的 DTB 物理地址。
    # 先扣在 s1 里、等 bss 清零之后再落盘——dtb_phys_addr 在 .bss 里，
    # 现在写进去会被下面的清零循环抹掉。
    mv    s1, a1
    # 记下引导核的 hartid，C 侧探测其余 hart 时要跳过它
    la    t0, boot_hartid_raw
    sd    a0, 0(t0)

    call  setup_boot_stack              # 相对寻址设置sp（低物理地址）

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

    # bss 已清零，现在才能把 DTB 地址存进全局。MMU 未开，la 取到的是物理地址。
    la    t0, dtb_phys_addr
    sd    s1, 0(t0)

    call  os_init_before_mmu_enable
    li    t0, SATPMODE_RV39
    la    t1, vmm_kernel_pgd_ppn
    ld    t1, 0(t1)
    or    a0, t0, t1                    # 计算satp寄存器值存入a0：SATPMODE_RV39 | kernel_pgd_ppn
    la    t0, _trampoline_start
    jr    t0

loop1:   j     loop1                    # enter the infinite loop

secondary_cpu_entry:
    # 经 HSM sbi_hart_start 启动，进入时 a0=hartid、**a1=opaque=我们传的逻辑 cpu 号**，
    # 且引导核已备好内核页表。建栈 + 走 trampoline 使能 MMU 跳高地址。
    # 用 opaque 而不是 hartid 当逻辑号，是因为 hartid 在目标平台上不连续也不从 0 起。
    mv    s0, a1
    call  setup_boot_stack
    li    t0, SATPMODE_RV39
    la    t1, vmm_kernel_pgd_ppn
    ld    t1, 0(t1)
    or    a0, t0, t1
    la    t0, _trampoline_start
    jr    t0

loop2:   j     loop2                    # enter the infinite loop

    # 按 s0（逻辑 cpu 号）算出该核的引导栈顶并装进 sp。
    # sp = boot_stacks + (s0 + 1) * BOOT_STACK_SIZE —— 栈由高向低增长，所以取的是
    # 下一格的起始地址。用移位而不是乘法：这里还没有 C 环境，而 rv64imafdc 的乘法
    # 指令虽然有，但保持与栈大小必须是 2 的幂这个约束一致更不容易写错。
    # 用 t0/t1 与 ra：调用点都在还没有栈的时刻，ra 由 call/ret 自己管，不碰 s 寄存器。
    .globl setup_boot_stack
    .align 2
setup_boot_stack:
    la    t0, boot_stacks
    addi  t1, s0, 1
    slli  t1, t1, BOOT_STACK_SHIFT
    add   sp, t0, t1
    ret

    .section .text.entry
    .globl cpu_get_core_id_asm
    .align 2 # 跳转/函数入口 这种取址访问要求对齐到4字节
cpu_get_core_id_asm:
    mv    a0, tp
    ret

    .globl cpu_set_core_id_asm
    .align 2
cpu_set_core_id_asm:
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
    call  setup_boot_stack              # 重新设置 sp（这次 la 取到的是高虚拟地址）
    mv    a0, s0                        # 将**逻辑 cpu 号**传给 os_init_after_mmu_enable
    call  os_init_after_mmu_enable

    # 引导核抢占标志。**必须放 .data**：.bss 会被上面的清零循环抹掉，
    # 那样后来的从核会重新抢到、把自己也当成引导核。
    .section .data
    .align 3
boot_hart_claimed:
    .dword 0
    .globl boot_hartid_raw
boot_hartid_raw:
    .dword 0

    # 每核一个引导栈，按逻辑 cpu 号索引。堆栈增长方向为由高地址向低地址增长，
    # 所以逻辑 cpu N 用的是 [N*SIZE, (N+1)*SIZE) 这一格的**高端**。
    .section .bss.stack
    # 根据 RISC-V psABI（调用约定）规范：函数调用时 sp 中的地址值必须 16 字节对齐；
    # BOOT_STACK_SIZE 是 2 的幂且远大于 16，按栈大小对齐自然满足。
    .align BOOT_STACK_SHIFT
    .globl boot_stacks
boot_stacks:
    .space BOOT_STACK_SIZE * CORE_NUMBER
    .globl boot_stacks_end
boot_stacks_end:
