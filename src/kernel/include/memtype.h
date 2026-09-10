#ifndef _MEMTYPE_H_
#define _MEMTYPE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "encoding.h"

/**
 * RISC-V uses 39-bit virtual address to access 56-bit physical address!
 * Sv39 virtual address:
 * +----9----+----9---+----9---+---12--+
 * |  VPN[2] | VPN[1] | VPN[0] | PGOFF |
 * +---------+----+---+--------+-------+
 * 
 * Sv39 physical address:
 * +----26---+----9---+----9---+---12--+
 * |  PPN[2] | PPN[1] | PPN[0] | PGOFF |
 * +---------+----+---+--------+-------+
 *
 * Sv39 page table entry: PTE中存储的永远是物理页号而不是虚拟内存
 * +----26---+----9---+----9---+---2----+-------8-------+
 * |  PPN[2] | PPN[1] | PPN[0] |Reserved|D|A|G|U|X|W|R|V|
 * +---------+----+---+--------+--------+---------------+
 */

#define PGSHIFT RISCV_PGSHIFT
#define PGSIZE RISCV_PGSIZE
#define PTE_PPN_OFFSET 10
#define PGD_OFFSET 30   /* Sv39虚拟地址中的VPN[2]部分的偏移量 */
#define PMD_OFFSET 21   /* Sv39虚拟地址中的VPN[1]部分的偏移量 */
#define PTE_OFFSET 12   /* Sv39虚拟地址中的VPN[0]部分的偏移量，易与页表项PTE混淆 */

#define PGD(va) (((va) >> PGD_OFFSET) & 0x1FF)
#define PMD(va) (((va) >> PMD_OFFSET) & 0x1FF)
#define PTE(va) (((va) >> PTE_OFFSET) & 0x1FF)

/*
 * 链接脚本导出的内核镜像边界符号。
 */
extern char skernel[];   /* 内核镜像起始 */
extern char ekernel[];   /* 内核镜像结束，随编译产物大小变化 */
extern char etext[];     /* .text 段结束 */
extern char erodata[];   /* .rodata 段结束 */
extern char edata[];     /* .data 段结束 */
extern char ebss[];      /* .bss 段结束 */

/* 内核镜像物理加载地址，**必须与对应平台链接脚本的 PHYS_BASE_ADDRESS 逐字一致**。
 * 用绝对值而不是 skernel：开启 MMU 后 skernel 变成高虚拟地址，拿它算物理内存布局全错。
 *
 * VF2 取 0x40200000 = DDR 基址 0x40000000 + 2 MB，这个 2 MB 偏移不是随便选的——
 * 它就是 Image 头里的 text_offset，`booti` 按"RAM 基址 + text_offset"决定把内核搬到哪。
 * 两者必须一致，否则 U-Boot 把内核放到 A 处、内核却按 B 处链接，一执行就跑飞。 */
#if defined(QEMU)
#define KERNEL_START ((phyAddr_t)(0x80200000))
#elif defined(VF2)
#define KERNEL_START ((phyAddr_t)(0x40200000))
#else
#error "未知平台：KERNEL_START 没有对应取值（PLATFORM 只允许 QEMU / VF2）"
#endif
/* 内核占用物理内存的末端，必须从链接符号读取。 */
#define KERNEL_END ((phyAddr_t)ekernel)

/* 物理内存布局（QEMU virt）：
 *
 *   0x80000000  ┌────────────────┐  RAM 起点，RustSBI 占 0x80000000~0x80200000
 *   0x80200000  ├────────────────┤  KERNEL_START：内核镜像 + PMM 页帧元数据
 *               │   PMM 页帧池    │
 *   0x87000000  ├────────────────┤  MEMORY_END：PMM 管到这里为止（不含）
 *               │  rootfs 预留区  │  ROOTFS_PHYS_BASE，QEMU -device loader 装到这里
 *   0x88000000  └────────────────┘  RAM 终点，见 scripts/run.sh 的 -m
 *
 * rootfs 预留区**故意放在 MEMORY_END 之外**：PMM 的页帧池只覆盖
 * [KERNEL_START, MEMORY_END)，天然就不会把这块编进去，pmm.c 一行都不用改。
 * 这比在池子中间挖洞简单得多，也不容易差一页。
 *
 * 改这里任何一个值都要同步改 scripts/run.sh 与 scripts/forgdb.sh 的 -m，
 * 三者对不上时 QEMU 只会静默给出更小的 RAM，越界访问要到很后面才暴露。
 *
 * VF2(JH7110) 用**同一个形状平移**：DDR 基址 0x4000_0000，各段偏移与 QEMU 侧一一对应
 * （0x8000_0000 → 0x4000_0000）。板子有 4 GB，这里只管 110 MB 是刻意的——
 * 放开到 GB 级要先做 2 MB 大页线性映射，否则 init_kernel_offset_mapping() 会逐个
 * 4 KB 页建 PTE，光页表就要几 MB、还要跑上百万次三级 walk。见计划文档 §2.2。
 *
 *   VF2:  0x40000000 RAM 起点 │ 0x40200000 KERNEL_START │ 0x47000000 MEMORY_END
 *         0x47000000 rootfs 预留区 │ 0x48000000 KERNEL_MAP_END
 *
 * ⚠️ U-Boot 的几个默认暂存地址实测（2023-02 SDK 固件）与上面的区间**是重叠的**：
 *     kernel_addr_r  = 0x40200000  恰好等于 KERNEL_START，booti 源即目标，无妨
 *     fdt_addr_r     = 0x46000000  落在页帧池内 ✗
 *     ramdisk_addr_r = 0x46100000  落在页帧池内 ✗
 * 所以加载时**不要用后两个变量**：rootfs 直接写死 0x47000000，DTB 用
 * fdtcontroladdr（0xfffc56a0，在 KERNEL_MAP_END 之外）。rootfs 是我们自己按地址
 * 预载的，撞上了不会有任何报错，只会静默改坏内存。命令见 tools/build_vf2_image.sh。 */
#if defined(QEMU)
#define MEMORY_END ((phyAddr_t)(0x87000000))
#define ROOTFS_PHYS_BASE ((phyAddr_t)(0x87000000))
#elif defined(VF2)
#define MEMORY_END ((phyAddr_t)(0x47000000))
#define ROOTFS_PHYS_BASE ((phyAddr_t)(0x47000000))
#else
#error "未知平台：MEMORY_END 没有对应取值（PLATFORM 只允许 QEMU / VF2）"
#endif
#define ROOTFS_MAX_SIZE ((uint64_t)(16 * 1024 * 1024))

/* 内核偏移映射要覆盖到哪里。**不等于 MEMORY_END**——PMM 的页帧池止于 MEMORY_END，
 * 但 rootfs 预留区在它之外，而 diskio.c 要拿 pa_to_kva() 直接读写那块内存。
 * 映射不延长的话，第一次 disk_read 就是一发 S 态缺页（且那块地址没有 VMA，
 * 会一路走到 panic）。
 * 分成两个常量而不是把 MEMORY_END 直接抬到 0x88000000：
 * "PMM 能分配的范围"和"内核能用 KVA 访问的范围"本来就是两件事，
 * 混成一个会让预留区重新落进页帧池。 */
#define KERNEL_MAP_END ((phyAddr_t)(ROOTFS_PHYS_BASE + ROOTFS_MAX_SIZE))
/* 内核高位虚拟地址偏移 */
#define KERNEL_VA_OFFSET 0xffffffc000000000UL /* Sv39 内核高地址区起始 */

typedef uintptr_t phyAddr_t;    /* 一个SV39物理地址 */
typedef uintptr_t virAddr_t;    /* 一个SV39虚拟地址 */
typedef uint64_t ppn_t;
typedef uint64_t pte_t;
typedef uint16_t pteflg_t;

/* 叶 PTE 一律补上 A 位，可写的再补 D 位。
 *
 * **A/D 的硬件自动置位是可选特性（Svadu 扩展），不能假定存在。**
 * QEMU 8.2 的 virt 带 svadu（isa 串里能看到），访问 A=0 的页硬件会自己补上；
 * VF2 的 U74 没有（`rv64imafdcbsux`），访问 A=0 的页**直接抛 page fault**。
 * 后果在开机时最致命：建内核页表时若不带 A，satp 一写就是取指 fault，
 * 而那一刻 stvec 还是 0、trap_init 尚未运行，于是 fault→跳 0→再 fault，
 * 串口连一个字都出不来，从现象上完全看不出是 A 位的问题。
 *
 * 判据取 R/W/X 是否有任一置位：Sv39 里这三位全 0 的 PTE 指向下一级页表，
 * 那种非叶 PTE 的 A/D 位没有意义，不能乱补。
 *
 * 代价是**放弃 A/D 位的语义**——将来若要做时钟置换算法，靠"清 A 位再看硬件是否
 * 重新置位"这条路在本平台上本来就走不通（见 vmm_probe_pte_ad 的探针），
 * 需要改用软件方式在缺页处理里维护。 */
static inline pte_t pte_create(ppn_t ppn, pteflg_t pteFlag)
{
    if ((pteFlag & (PTE_R | PTE_W | PTE_X)) != 0)
    {
        pteFlag |= PTE_A;
        if ((pteFlag & PTE_W) != 0)
        {
            pteFlag |= PTE_D;
        }
    }
    return (pte_t)((ppn << PTE_PPN_OFFSET) | pteFlag | PTE_V);
}

static inline pteflg_t pte_get_flag(pte_t pte)
{
    return (pteflg_t)(pte & (pte_t)0xFF);
}

/* Return true if(PTE_V & pte).  */
static inline bool pte_is_valid(pte_t pte)
{
    return (pte & PTE_V) != 0x0;
}

static inline bool pte_is_readable(pte_t pte)
{
      return (pte & PTE_R) != 0x0;
}

static inline bool pte_is_writable(pte_t pte)
{
    return (pte & PTE_W) != 0x0;
}

static inline bool pte_is_executable(pte_t pte)
{
    return (pte & PTE_X) != 0x0;
}

/**
 * @name tlb_flush_all
 * @brief 刷新全部tlb缓存
 */
static inline void tlb_flush_all(void)
{
    asm volatile("sfence.vma");
}

/** 
 * @name tlb_flush_va
 * @brief 刷新指定虚拟地址的tlb缓存
 * @param va 需要刷新的虚拟地址
 * @details sfence.vma有两个参数rs1和rs2：sfence.vma rs2, rs1
 * rs1: 需要刷新的虚拟地址
 * rs2: 需要刷新的ASID，ASID是进程地址空间标识符。便于操作系统使用
 * 两者都为x0（0）时，刷新所有tlb
 */
static inline void tlb_flush_va(virAddr_t va)
{
    asm volatile("sfence.vma %0"
                 :
                 : "r"(va));
}

/* 内核物理地址转虚拟地址 */
static inline virAddr_t pa_to_kva(phyAddr_t pa)
{
    return (virAddr_t)(pa + KERNEL_VA_OFFSET);
}

/* 内核虚拟地址转物理地址 */
static inline phyAddr_t kva_to_pa(virAddr_t va)
{
    return (phyAddr_t)(va - KERNEL_VA_OFFSET);
}

/* satp.MODE != 0 表示 MMU 已启用（Sv39 时 MODE = 8） */
static inline bool mmu_is_enabled(void)
{
    return (read_csr(satp) >> 60) != 0;
}

#endif
