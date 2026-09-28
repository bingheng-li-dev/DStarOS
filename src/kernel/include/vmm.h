/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _VMM_H_
#define _VMM_H_

#include "pmm.h"
#include "list.h"
#include "debug.h"
#include "uaccess.h"

/**
 * satp 寄存器模式位
 * SATP register:
 * +---4--+----16----+-------44-------+
 * | MODE |   ASID   |       PPN      |
 * +------+----------+----------------+
 */
#define SATPMODE_BARE 0x0000000000000000UL
#define SATPMODE_RV39 0x8000000000000000UL

/* VMA 保护标志，与 PTE_R/W/X 对应，通过 vma_prot_to_pte_flags 转换 */
#define VMP_R 0x1
#define VMP_W 0x2
#define VMP_X 0x4
#define VMA_HEAP 0x8 /* 代表堆区，特殊处理 */

typedef uint16_t pgprot_t;
typedef struct vm_area_struct vma_t;
typedef struct mm_struct mm_t;

/* 进程虚拟地址空间描述符 */
struct mm_struct
{
    uint16_t map_count;             /* vma 数量 */
    ppn_t pgd_ppn;                  /* 进程一级页表帧 */
    virAddr_t brk_start;            /* 堆起始地址 */
    virAddr_t brk_current;          /* 堆当前地址 */
    struct list_head mmap_list;     /* VMA 链表头 */
};

/* 连续虚拟地址区间描述符 */
struct vm_area_struct
{
    mm_t *proc_mm;              /* 所属进程的 mm */
    virAddr_t vm_start;         /* 区间起始虚拟地址（含） */
    virAddr_t vm_end;           /* 区间结束虚拟地址（不含） */
    pgprot_t vm_flag;           /* 保护标志：VMP_R/W/X 组合 */
    struct list_head vma_list_linker;
};

extern ppn_t vmm_kernel_pgd_ppn;

void   vmm_init(void);
void   vmm_remove_identity_mapping(void);
int    vmm_map_2m_page(ppn_t pgd_ppn, virAddr_t va, phyAddr_t pa, pteflg_t flags, bool mmu_enabled);
void   vmm_map_mmio_range(phyAddr_t pa_start, phyAddr_t pa_end);
mm_t  *vmm_mm_create(void);
int    vmm_mm_alloc_pgd(mm_t *mm);
void   vmm_mm_destroy(mm_t *mm);
int    vmm_mm_copy(mm_t *dst, mm_t *src);
vma_t *vmm_vma_create(virAddr_t va_start, virAddr_t va_end, pgprot_t flag);
void   vmm_vma_destroy(vma_t *vma);
vma_t *vmm_vma_get(mm_t *mm, virAddr_t va);
void   vmm_vma_insert(mm_t *mm, vma_t *vma);
int    vmm_map_vma(mm_t *mm, vma_t *vma);
int    vmm_map_fixed_page(mm_t *mm, virAddr_t va, ppn_t ppn, pgprot_t prot);
void   vmm_unmap_vma(mm_t *mm, vma_t *vma);
void   vmm_unmap_range(mm_t *mm, virAddr_t start, virAddr_t end);
virAddr_t vmm_mmap_find_free_area(mm_t *mm, uint64_t len);
void   vmm_page_fault_handler(virAddr_t badva, int fault_type);

#endif /* _VMM_H_ */
