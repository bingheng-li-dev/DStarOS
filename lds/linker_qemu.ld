OUTPUT_ARCH(riscv)
ENTRY(_start)
/* 物理基地址 */
PHYS_BASE_ADDRESS = 0x80200000;
/* 需要与src/kernel/include/memtype.h 中的 KERNEL_VA_OFFSET 保持一致 */
KERNEL_VA_OFFSET = 0xffffffc000000000;
/* 内核（虚拟）基地址 */
BASE_ADDRESS = PHYS_BASE_ADDRESS + KERNEL_VA_OFFSET;

SECTIONS
{
    . = BASE_ADDRESS;
    PROVIDE(skernel = .);

    /* AT() 指定 section 的加载地址（LMA）与运行地址（VMA）分离
        每个段加 `AT(VMA - KERNEL_VA_OFFSET)`，告诉链接器把数据写到低物理地址（LMA），
        代码中所有符号的地址（VMA）是高地址。QEMU 从 LMA 加载，执行时 MMU 翻译到 VMA */
    .text : AT(ADDR(.text) - KERNEL_VA_OFFSET) {
        *(.text.entry)
        *(.text.trampoline)
        *(.text .text.*)
    }
    . = ALIGN(4K);
    PROVIDE(etext = .);

    .rodata : AT(ADDR(.rodata) - KERNEL_VA_OFFSET) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }
    . = ALIGN(4K);
    PROVIDE(erodata = .);

    .data : AT(ADDR(.data) - KERNEL_VA_OFFSET) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }
    . = ALIGN(4K);
    PROVIDE(edata = .);

    .bss : AT(ADDR(.bss) - KERNEL_VA_OFFSET) {
        *(.bss.stack)
        sbss = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
    }
    . = ALIGN(4K);
    PROVIDE(ebss = .);

    PROVIDE(ekernel = .);

    /DISCARD/ : {
        *(.eh_frame)
    }
}