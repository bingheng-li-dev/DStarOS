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
    /* 加上 SIZEOF_HEADERS，让第一个 PT_LOAD 从文件偏移 0 起、把 ELF 头与程序头表
     * 一起映射进来。不这么做的话第一个段从 p_offset=0x1000 开始，e_phoff(64) 落在
     * 任何已加载段之外，elf_load 求不出程序头表的虚拟地址，AT_PHDR/AT_PHENT/AT_PHNUM
     * 只能一起填 0——musl 的 __init_tls 就找不到 PT_TLS 段了。
     * 真实可执行文件（含 musl 链出来的 BusyBox）都是这个布局。 */
    . = BASE_ADDRESS + SIZEOF_HEADERS;

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
