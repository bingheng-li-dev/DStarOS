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

/* 内核镜像物理加载地址，由 RustSBI / QEMU virt 平台固定，必须与链接脚本 PHYS_BASE_ADDRESS 一致。 */
#define KERNEL_START ((phyAddr_t)(0x80200000)) /* 使用绝对值而非skernel，避免开启MMU后计算整个内存布局错误 */
/* 内核占用物理内存的末端，必须从链接符号读取。 */
#define KERNEL_END ((phyAddr_t)ekernel)
/* k210物理内存末端 */
#define MEMORY_END ((phyAddr_t)(0x80800000))
/* 内核高位虚拟地址偏移 */
#define KERNEL_VA_OFFSET 0xffffffc000000000UL /* Sv39 内核高地址区起始 */

typedef uintptr_t phyAddr_t;    /* 一个SV39物理地址 */
typedef uintptr_t virAddr_t;    /* 一个SV39虚拟地址 */
typedef uint64_t ppn_t;
typedef uint64_t pte_t;
typedef uint16_t pteflg_t;

static inline pte_t pte_create(ppn_t ppn, pteflg_t pteFlag)
{
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
