OUTPUT_ARCH(riscv)
ENTRY(_start)
/* 物理基地址 = JH7110 的 DDR 基址 0x40000000 + 2 MB。
 * 这个 2 MB 不是随便选的：它就是 startup.S 那个 Image 头里的 text_offset，
 * `booti` 按"RAM 基址 + text_offset"决定把内核搬到哪。
 * 三处必须逐字一致——本文件、Image 头的 text_offset、memtype.h 的 KERNEL_START。 */
PHYS_BASE_ADDRESS = 0x40200000;
/* 需要与src/kernel/include/memtype.h 中的 KERNEL_VA_OFFSET 保持一致 */
KERNEL_VA_OFFSET = 0xffffffc000000000;
/* 内核（虚拟）基地址。高半区从 0xffffffc000000000 起有 256 GB，
 * 换 PA 基址不影响这套偏移映射，vmm.c 一行不用改。 */
BASE_ADDRESS = PHYS_BASE_ADDRESS + KERNEL_VA_OFFSET;

SECTIONS
{
    . = BASE_ADDRESS;
    PROVIDE(skernel = .);

    /* AT() 指定 section 的加载地址（LMA）与运行地址（VMA）分离
        每个段加 `AT(VMA - KERNEL_VA_OFFSET)`，告诉链接器把数据写到低物理地址（LMA），
        代码中所有符号的地址（VMA）是高地址。 */
    .text : AT(ADDR(.text) - KERNEL_VA_OFFSET) {
        /* **Image 头必须是镜像的第 0 字节**：booti 搬完之后直接跳到这里，
         * 第一条指令是头里的 code0（一条跳到 _start 的 j）。
         * 放错位置的表现是 U-Boot 报 "Bad Linux RISCV Image magic!"。 */
        *(.image_header)
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
