/* 用户态程序链接脚本：入口从 0x10000 起，各段按页对齐分隔。
 * 段间显式 ALIGN(4K) 是为了让不同权限的段（R+X 的 .text、R 的 .rodata、
 * R+W 的 .data/.bss）各自独占物理页——避免 elf.c 里记录的已知限制
 * （相邻 PT_LOAD 段共享同一物理页时 vmm_map_vma 会互相覆盖数据）。
 * 不需要内核链接脚本里 trampoline/AT(LMA) 那套偏移映射技巧：
 * 用户程序是普通可执行文件，VA 与文件内布局的对应关系由 elf_load 直接按
 * p_vaddr/p_offset 处理，无需内核那种"加载地址 != 运行地址"的区分。 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

BASE_ADDRESS = 0x10000;

SECTIONS
{
    . = BASE_ADDRESS;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    . = ALIGN(4096);
    .bss : {
        sbss = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        ebss = .;
    }

    . = ALIGN(4096);

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes) }
}
