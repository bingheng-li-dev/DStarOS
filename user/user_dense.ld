/* 用户态"紧凑布局"链接脚本 —— 只给 user/segtest.c 用。
 *
 * 与 user.ld 的唯一区别：**故意去掉段间的 ALIGN(4096)**，让 R+X 的 .text/.rodata
 * 与 R+W 的 .data/.bss 首尾相接。配合 user/Makefile 里的 -z max-page-size=16，
 * 链接器不再把两个 PT_LOAD 各自推到页边界上，于是它们共用中间那一页。
 *
 * 这正是 user.ld 的注释里提到、elf_load 早期不处理的那个场景。真实工具链
 * （含 musl 链出来的 BusyBox）是否共享页取决于链接器默认行为，内核不能依赖它，
 * 所以这里把最坏情况固定下来当回归用例。
 *
 * 除 segtest 外的所有用户程序仍用 user.ld，不要改用本脚本。 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

BASE_ADDRESS = 0x10000;

/* 必须显式声明 PHDRS 并把 section 逐个指派过去。只去掉 ALIGN 是不够的——
 * 段边界不再页对齐之后，ld 的分段启发式会直接把 R+X 与 R+W **合并成一个 RWE
 * 段**，共享页的场景反而消失了（实测各种 -z max-page-size 都拦不住）。
 * 写死两个 PT_LOAD 才能让布局不依赖链接器的判断。 */
PHDRS
{
    /* FILEHDR PHDRS：把 ELF 头与程序头表并进第一个 PT_LOAD，让它从文件偏移 0 起。
     * 不写的话第一个段从 p_offset=0xb0 开始，程序头表落在任何已加载段之外，
     * AT_PHDR 求不出来——那是 user.ld 里 SIZEOF_HEADERS 早就修过的老问题，
     * 显式 PHDRS 下 SIZEOF_HEADERS 管不到，得在这里再声明一次。 */
    text PT_LOAD FILEHDR PHDRS FLAGS(5); /* PF_R | PF_X */
    data PT_LOAD FLAGS(6);               /* PF_R | PF_W */
}

SECTIONS
{
    /* 与 user.ld 一致：加上 SIZEOF_HEADERS 让第一个 PT_LOAD 从文件偏移 0 起，
     * 把 ELF 头与程序头表一起映射进来，AT_PHDR 才求得出。 */
    . = BASE_ADDRESS + SIZEOF_HEADERS;

    .text : {
        *(.text.entry)
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :text

    .data : {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    .bss : {
        sbss = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        ebss = .;
    } :data

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.riscv.attributes) }
}
