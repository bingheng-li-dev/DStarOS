/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "vmm.h"
#include "slab.h"
#include "errorcode.h"
#include "kmalloc.h"
#include "console.h"
#include "proc.h"
#include "stringops.h"
#include "cpu.h"
#include "sync.h"
#include "periph_layout.h"
#include "startup.h"
#include "probes.h"

/**
 * @brief 内核页表根目录的物理页号。
 * @details Sv39 一页 512 个页表项：[0,255] 对应低半区虚拟地址，归用户空间；
 *   [256,511] 对应 0xFFFFFFC000000000 以上，归内核高位映射。
 */
ppn_t vmm_kernel_pgd_ppn;

/**
 * @brief 保护跨进程共享物理帧（COW）状态的全局锁
 * @details 覆盖 pframe_t.reference 的读-判断-改与伴随的 PTE 改写：fork、缺页拆分、
 *   进程退出三处都会摸同一批共享帧，不加锁会丢更新，页框被提前释放却还有 PTE 指向它。
 * @note 锁序：本锁总是外层，内部的 pmm_lock 是内层，只在这个方向嵌套。
 */
osslock_t vmm_lock;

/**
 * @brief 获取给定虚拟地址在 SV39 三级页表中对应的三级页表项指针（vmm 模块内部使用）
 * @param[in] pgd_ppn     一级页表（PGD）根帧的物理页号，即 satp 寄存器 PPN 域的值
 * @param[in] va          需要查询/创建映射的虚拟地址
 * @param[in] create      若为 true，则在中间节点不存在时自动分配新帧并建立指针；
 *                        若为 false，则仅做只读查询，任意层缺失即返回 NULL
 * @param[in] mmu_enabled 若为 true，表示 MMU 已启用，访问页表帧时需经 pa_to_kva() 转换；
 *                        若为 false（MMU 未启用阶段），物理地址可直接作为指针使用
 * @return 指向三级页表项（PTE）的指针；若对应条目不存在且 create 为 false，则返回 NULL
 * @note 三级页表数组 pgd/pmd/pte 中每个条目存储的是完整 PTE（含 PPN 与标志位），
 *       而非仅 PPN。中间节点 PTE 的 R/W/X 位均为 0，叶子节点 PTE 至少有一位为 1。
 */
static pte_t *get_pte(ppn_t pgd_ppn, virAddr_t va, bool create, bool mmu_enabled)
{
    ppn_t pgd_idx = PGD(va);
    ppn_t pmd_idx = PMD(va);
    ppn_t pte_idx = PTE(va);

    phyAddr_t pgd_pa = convert_ppn2pa(pgd_ppn);
    pte_t *pgd = mmu_enabled ? (pte_t *)pa_to_kva(pgd_pa) : (pte_t *)pgd_pa;
    if (!pte_is_valid(pgd[pgd_idx]))
    {
        if (!create)
        {
            return NULL;
        }

        pframe_t *new_pmd_frame = pmm_alloc_page();
        if (!new_pmd_frame)
        {
            return NULL;
        }
        new_pmd_frame->reference += 1;
        pgd[pgd_idx] = pte_create(convert_pframe2ppn(new_pmd_frame), 0);
    }

    phyAddr_t pmd_pa = convert_ppn2pa(pgd[pgd_idx] >> PTE_PPN_OFFSET);
    pte_t *pmd = mmu_enabled ? (pte_t *)pa_to_kva(pmd_pa) : (pte_t *)pmd_pa;
    if (!pte_is_valid(pmd[pmd_idx]))
    {
        if (!create)
        {
            return NULL;
        }

        pframe_t *newPte_frame = pmm_alloc_page();
        if (!newPte_frame)
        {
            return NULL;
        }
        newPte_frame->reference += 1;
        pmd[pmd_idx] = pte_create(convert_pframe2ppn(newPte_frame), 0);
    }

    phyAddr_t pte_pa = convert_ppn2pa(pmd[pmd_idx] >> PTE_PPN_OFFSET);
    pte_t *pte = mmu_enabled ? (pte_t *)pa_to_kva(pte_pa) : (pte_t *)pte_pa;

    if (!create && !pte_is_valid(pte[pte_idx]))
    {
        return NULL;
    }
    return &pte[pte_idx];
}

/**
 * @brief 建立内核高位偏移映射（Offset Mapping），将全部物理内存线性映射到高虚拟地址
 * @details 映射关系 VA = PA + KERNEL_VA_OFFSET，按链接脚本的分段给不同权限。
 *   另为 trampoline 段建临时恒等映射（VA = PA），使 satp 写完、PC 还在低地址时仍能翻译；
 *   该映射由 vmm_remove_identity_mapping() 撤销。
 * @note 在 MMU 启用前调用，页表帧一律按物理地址访问（get_pte 的 mmu_enabled 传 false）。
 */
static void init_kernel_offset_mapping(void)
{
    pframe_t *kernel_pgd_pframe = pmm_alloc_page();
    if (!kernel_pgd_pframe)
    {
        panic("Failed to allocate page for kernel PGD!\n");
    }
    memset((void *)convert_pframe2pa(kernel_pgd_pframe), 0, PGSIZE);
    kernel_pgd_pframe->reference += 1;

    vmm_kernel_pgd_ppn = convert_pframe2ppn(kernel_pgd_pframe);

    ppn_t ppn_skernel  = convert_pa2ppn_flr(KERNEL_START);
    ppn_t ppn_etext    = convert_pa2ppn_flr((phyAddr_t)etext);
    ppn_t ppn_erodata  = convert_pa2ppn_flr((phyAddr_t)erodata);
    /* 映射到 KERNEL_MAP_END 而不是 MEMORY_END：rootfs 预留区在 PMM 页帧池之外，
     * 但 diskio.c 要拿 pa_to_kva() 直接读写它，不映射的话第一次 disk_read 就是
     * 一发落在无 VMA 区域的 S 态缺页，一路走到 panic。 */
    ppn_t ppn_end      = convert_pa2ppn_cil(KERNEL_MAP_END);

    /** 偏移映射：VA = PA + KERNEL_VA_OFFSET → PA，按段分配权限
     * 由ld文件，内存布局为skernel → text → rodata → data → bss → ekernel → 其他物理内存
     */
    for (ppn_t ppn = ppn_skernel; ppn < ppn_etext; ppn++)
    {
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, pa_to_kva(convert_ppn2pa(ppn)), true, false);
        *pte = pte_create(ppn, PTE_G | PTE_R | PTE_X);
    }
    for (ppn_t ppn = ppn_etext; ppn < ppn_erodata; ppn++)
    {
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, pa_to_kva(convert_ppn2pa(ppn)), true, false);
        *pte = pte_create(ppn, PTE_G | PTE_R);
    }
    /* 直到 KERNEL_MAP_END 的物理内存全部映进高 VA，内核随时可用 pa_to_kva(pa) 访问任意
     * 物理帧（例如 COW 复制内容）；用户帧因此有两个映射，内核页表一个、用户页表一个。
     * PTE_G 表示该项对所有 ASID 有效，只有 sfence.vma zero, zero 才清得掉。 */
    for (ppn_t ppn = ppn_erodata; ppn < ppn_end; ppn++)
    {
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, pa_to_kva(convert_ppn2pa(ppn)), true, false);
        *pte = pte_create(ppn, PTE_G | PTE_R | PTE_W);
    }

    /* 临时恒等映射，只覆盖 trampoline 所在页：enable_mmu 后 PC 还在低地址，要能翻译。
     * MMU 关闭时 auipc 相对寻址得到的就是物理地址，所以这里不能再套 kva_to_pa()——
     * 那会再减一次 KERNEL_VA_OFFSET。vmm_remove_identity_mapping() 在 MMU 开启后调用，
     * 拿到的才是 VA，那边用 kva_to_pa 才正确。 */
    phyAddr_t tramp_pa_start = (phyAddr_t)_trampoline_start;
    phyAddr_t tramp_pa_end   = (phyAddr_t)_trampoline_end;
    for (ppn_t ppn = convert_pa2ppn_flr(tramp_pa_start); ppn < convert_pa2ppn_cil(tramp_pa_end); ppn++)
    {
        /* trampoline段需要VA = PA，因此pa不需要转换为kva，pa就是kva */
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, convert_ppn2pa(ppn), true, false);
        *pte = pte_create(ppn, PTE_G | PTE_R | PTE_X);
    }
}

/**
 * @brief 在指定页表里建立一条 2 MB 大页映射（Sv39 第 1 级叶 PTE）
 * @param[in] pgd_ppn     根页表帧的物理页号
 * @param[in] va          目标虚拟地址，必须按 2 MB 对齐
 * @param[in] pa          目标物理地址，必须按 2 MB 对齐
 * @param[in] flags       叶 PTE 的权限位组合，A/D 位由 pte_create() 统一补
 * @param[in] mmu_enabled 若为 true，访问页表帧时需经 pa_to_kva() 转换
 * @retval ENO0_NO_ERROR    映射建立成功
 * @retval ENO6_INVAL_PARAM va 或 pa 未按 2 MB 对齐
 * @retval ENO1_NOMORE_MEM  中间级页表帧分配失败
 * @retval ENO7_EXISTS      该 2 MB 区间已有映射（大页或下一级页表指针）
 * @details Sv39 的大页就是"中间级 PTE 直接当叶子"：第 1 级 PTE 的 R/W/X 只要有一位
 *   置上，硬件就不再把它当指针，而是按 2 MB 粒度解释它的 PPN——所以 PA 的低 21 位
 *   必须为 0，由入口的对齐校验保证。
 *
 *   不改 get_pte() 支持大页：它在本文件有二十来处调用，改返回语义等于把风险摊到所有路径。
 * @note 必须复用 pte_create() 而不是自己拼 PTE。叶 PTE 少了 A 位，在没有 Svadu
 *   扩展的 U74 上一访问就是 page fault，见 memtype.h 里 pte_create() 的注释。
 */
int vmm_map_2m_page(ppn_t pgd_ppn, virAddr_t va, phyAddr_t pa, pteflg_t flags, bool mmu_enabled)
{
    if ((va & (PGSIZE_2M - 1)) != 0 || (pa & (PGSIZE_2M - 1)) != 0)
    {
        return ENO6_INVAL_PARAM;
    }

    phyAddr_t pgd_pa = convert_ppn2pa(pgd_ppn);
    pte_t *pgd = mmu_enabled ? (pte_t *)pa_to_kva(pgd_pa) : (pte_t *)pgd_pa;
    ppn_t pgd_idx = PGD(va);

    if (!pte_is_valid(pgd[pgd_idx]))
    {
        pframe_t *new_pmd_frame = pmm_alloc_page();
        if (!new_pmd_frame)
        {
            return ENO1_NOMORE_MEM;
        }
        new_pmd_frame->reference += 1;
        pgd[pgd_idx] = pte_create(convert_pframe2ppn(new_pmd_frame), 0);
    }
    else if (pte_is_readable(pgd[pgd_idx]) || pte_is_writable(pgd[pgd_idx]) ||
             pte_is_executable(pgd[pgd_idx]))
    {
        return ENO7_EXISTS; /* 这一项已是 1 GB 大页的叶 PTE，不能再往下走 */
    }

    phyAddr_t pmd_pa = convert_ppn2pa(pgd[pgd_idx] >> PTE_PPN_OFFSET);
    pte_t *pmd = mmu_enabled ? (pte_t *)pa_to_kva(pmd_pa) : (pte_t *)pmd_pa;
    ppn_t pmd_idx = PMD(va);

    if (pte_is_valid(pmd[pmd_idx]))
    {
        return ENO7_EXISTS;
    }

    pmd[pmd_idx] = pte_create(convert_pa2ppn_flr(pa), flags);
    return ENO0_NO_ERROR;
}

/**
 * @brief 把一段设备寄存器区（MMIO）用 2 MB 大页映射进内核高半区
 * @param[in] pa_start 起始物理地址（含），必须按 2 MB 对齐
 * @param[in] pa_end   结束物理地址（不含），必须按 2 MB 对齐
 * @details 设备区沿用与 RAM 相同的偏移，pa_to_kva() 直接可用；两段地址不重叠，理由见
 *   memtype.h 的 MMIO_PHYS_BASE。权限固定 PTE_G | PTE_R | PTE_W：不给 PTE_X；
 *   cache 属性由 PMA 决定，Sv39 的 PTE 里没有对应位。
 * @note 在 MMU 启用前、且在 init_kernel_offset_mapping() 之后调用（内核根页表由它分配）。
 *   失败一律 panic：设备区映射不上，串口与 SD 驱动都起不来。
 */
void vmm_map_mmio_range(phyAddr_t pa_start, phyAddr_t pa_end)
{
    for (phyAddr_t pa = pa_start; pa < pa_end; pa += PGSIZE_2M)
    {
        int ret = vmm_map_2m_page(vmm_kernel_pgd_ppn, pa_to_kva(pa), pa,
                                  PTE_G | PTE_R | PTE_W, false);
        if (ret != ENO0_NO_ERROR)
        {
            panic("vmm_map_mmio_range: failed at pa 0x%lx, ret %d\n", (unsigned long)pa, ret);
        }
    }
}

/**
 * @brief 撤销内核页表中 trampoline 段的临时恒等映射（VA = PA）
 * @details MMU 启用后，PC 已跳转到高虚拟地址，trampoline 代码不再需要恒等映射。
 *   此函数逐页清除对应 PTE（置 0，即清除 PTE_V），并执行全局 TLB 刷新。
 * @note 必须在 MMU 启用、内核已运行在高 VA 之后调用；
 *       调用后任何通过低地址访问 trampoline 的操作都将触发页错误。
 */
void vmm_remove_identity_mapping(void)
{
    phyAddr_t tramp_pa_start = kva_to_pa((virAddr_t)_trampoline_start);
    phyAddr_t tramp_pa_end   = kva_to_pa((virAddr_t)_trampoline_end);
    for (ppn_t ppn = convert_pa2ppn_flr(tramp_pa_start); ppn < convert_pa2ppn_cil(tramp_pa_end); ppn++)
    {
        /* 不能pa_to_kva，要找的是恒等映射成VA的PA */
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, convert_ppn2pa(ppn), false, true);
        if (pte)
        {
            *pte = 0; /* 清 PTE_V，使该映射立即失效 */
        }
    }
    tlb_flush_all();
    printf("virtual memory management inited!\n");
}

/**
 * @brief 初始化虚拟内存管理模块
 * @note 必须在 PMM 初始化完成后、启用 MMU 之前调用。
 *   调用完成后应立即写入 satp 并执行 sfence.vma 以启用 MMU。
 */
void vmm_init(void)
{
    spinlock_init(&vmm_lock);
    init_kernel_offset_mapping();
    vmm_map_mmio_range(MMIO_PHYS_BASE, MMIO_PHYS_END);
}

/**
 * @brief 在进程地址空间中查找包含给定虚拟地址的 VMA
 * @param[in] mm 进程地址空间描述符
 * @param[in] va 要查找的虚拟地址
 * @return 包含 va 的 vma_t 指针；若不存在或 va 超出堆当前边界则返回 NULL
 * @note 对于 VMA_HEAP 类型的区间，即使 va 落在 [vm_start, vm_end) 内，
 *       若 va >= brk_current 也视为越界，返回 NULL。
 */
vma_t *vmm_vma_get(mm_t *mm, virAddr_t va)
{
    struct list_head *pos;
    list_for_each(pos, &mm->mmap_list)
    {
        vma_t *vma = list_entry(pos, vma_t, vma_list_linker);
        if (vma->vm_start <= va && vma->vm_end > va)
        {
            if (vma->vm_flag & VMA_HEAP && va >= mm->brk_current)
            {
                return NULL;
            }
            return vma;
        }
    }
    return NULL;
}

/**
 * @brief 分配并初始化一个进程地址空间描述符（mm_t）
 * @return 成功返回新的 mm_t 指针；内存不足返回 NULL
 * @note 这里只把 pgd_ppn 置成内核页表，用户进程的独立 PGD 由调用方再经 vmm_mm_alloc_pgd() 配上。
 */
mm_t *vmm_mm_create(void)
{
    mm_t *ret = slab_cache_alloc(mm_cache);
    if (ret != NULL)
    {
        ret->map_count   = 0;
        ret->pgd_ppn     = vmm_kernel_pgd_ppn;
        ret->brk_start   = 0;
        ret->brk_current = 0;
        INIT_LIST_HEAD(&ret->mmap_list);
    }
    return ret;
}

/**
 * @brief 给 mm 配一张独立的根页表：清零后复制内核半段，使新地址空间也能访问内核
 * @retval ENO0_NO_ERROR   成功，mm->pgd_ppn 指向新页表
 * @retval ENO1_NOMORE_MEM 分配不到页表帧，mm 不变
 */
int vmm_mm_alloc_pgd(mm_t *mm)
{
    pframe_t *frame = slab_alloc_page_retry();
    if (frame == NULL)
    {
        return ENO1_NOMORE_MEM;
    }
    pte_t *pgd = (pte_t *)convert_pframe2kva(frame);
    pte_t *kpgd = (pte_t *)pa_to_kva(convert_ppn2pa(vmm_kernel_pgd_ppn));
    memset(pgd, 0, PGSIZE);
    memcpy(pgd + PGD_KERNEL_START, kpgd + PGD_KERNEL_START,
           sizeof(pte_t) * (PGD_ENTRIES - PGD_KERNEL_START));
    frame->reference += 1;
    mm->pgd_ppn = convert_pframe2ppn(frame);
    return ENO0_NO_ERROR;
}

/**
 * @brief 分配并初始化一个虚拟内存区间描述符（vma_t）
 * @param[in] va_start 区间起始虚拟地址（含）
 * @param[in] va_end   区间结束虚拟地址（不含）
 * @param[in] flag     保护标志：VMP_R / VMP_W / VMP_X 的组合，可附加 VMA_HEAP
 * @return 成功返回新的 vma_t 指针；内存不足返回 NULL
 * @note 新建的 vma_t 未挂入任何 mm_t，proc_mm 字段为 NULL；
 *       需要调用 vmm_vma_insert() 将其关联到具体进程。
 */
vma_t *vmm_vma_create(virAddr_t va_start, virAddr_t va_end, pgprot_t flag)
{
    vma_t *ret = slab_cache_alloc(vma_cache);
    if (ret != NULL)
    {
        ret->vm_start = va_start;
        ret->vm_end   = va_end;
        ret->vm_flag  = flag;
        ret->proc_mm  = NULL;
        INIT_LIST_HEAD(&ret->vma_list_linker);
    }
    return ret;
}

/**
 * @brief 释放一个 vma_t 描述符的内存
 * @param[in] vma 要销毁的 VMA 指针（可为 NULL，此时函数无操作）
 * @note 此函数仅释放描述符本身，不解除对应的物理页映射；
 *       调用前应先通过 vmm_unmap_vma() 清除 PTE 并归还物理帧。
 */
void vmm_vma_destroy(vma_t *vma)
{
    if (vma != NULL)
    {
        kfree(vma);
    }
}

/**
 * @brief 将 vma_t 按 vm_start 升序插入进程地址空间的 VMA 链表
 * @param[in] mm  目标进程地址空间描述符
 * @param[in] vma 要插入的 VMA（调用前 vm_start/vm_end/vm_flag 已填写）
 * @note 插入后 vma->proc_mm 指向 mm，mm->map_count 递增 1。
 *   此函数不检查区间重叠，调用者需自行保证新区间与已有 VMA 不冲突。
 */
void vmm_vma_insert(mm_t *mm, vma_t *vma)
{
    vma->proc_mm = mm;
    struct list_head *pos;
    list_for_each(pos, &mm->mmap_list)
    {
        vma_t *cur = list_entry(pos, vma_t, vma_list_linker);
        if (vma->vm_start < cur->vm_start)
        {
            list_add_tail(&vma->vma_list_linker, pos);
            mm->map_count++;
            return;
        }
    }
    list_add_tail(&vma->vma_list_linker, &mm->mmap_list);
    mm->map_count++;
}

/**
 * @brief 在 mmap 区里找一段长度为 len 的空闲虚拟地址（first-fit）
 * @param[in] mm  目标进程地址空间描述符
 * @param[in] len 需要的长度（页对齐）
 * @retval 0 mmap 区已无足够大的空洞
 * @return 空洞起始虚拟地址
 * @details 从 USER_MMAP_BASE 起沿 mmap_list（按 vm_start 升序）线性扫描，
 *   逐个检查"游标到当前 VMA 起始"这段空洞够不够 len，不够就把游标推到该 VMA 末尾。
 * @note 用户栈自己也是一条 VMA，所以扫描不需要为它单独留边界；
 *   上界只用 USER_STACK_TOP 兜底。
 */
virAddr_t vmm_mmap_find_free_area(mm_t *mm, uint64_t len)
{
    virAddr_t cursor = USER_MMAP_BASE;
    struct list_head *pos;

    if (len == 0 || len > USER_STACK_TOP - USER_MMAP_BASE)
    {
        return 0;
    }

    list_for_each(pos, &mm->mmap_list)
    {
        vma_t *vma = list_entry(pos, vma_t, vma_list_linker);
        if (vma->vm_end <= cursor)
        {
            continue;
        }
        if (vma->vm_start >= cursor + len)
        {
            return cursor;
        }
        cursor = vma->vm_end;
    }

    if (cursor + len <= USER_STACK_TOP)
    {
        return cursor;
    }
    return 0;
}

/**
 * @brief 将 VMA 保护标志（pgprot_t）转换为用户空间 PTE 标志位（pteflg_t）
 * @param[in] prot VMA 保护标志，VMP_R / VMP_W / VMP_X 的任意组合
 * @return 对应的 PTE 标志位组合，始终包含 PTE_U（用户可访问）
 * @note 此函数仅用于用户空间映射；内核映射不应设置 PTE_U，需直接构造标志位。
 */
static pteflg_t vma_prot_to_pte_flags(pgprot_t prot)
{
    pteflg_t flags = PTE_U;
    if (prot & VMP_R) flags |= PTE_R;
    if (prot & VMP_W) flags |= PTE_W;
    if (prot & VMP_X) flags |= PTE_X;
    return flags;
}

/**
 * @brief 为 VMA 描述的整个虚拟地址区间建立物理页映射（即时分配）
 * @param[in] mm  所属进程地址空间描述符
 * @param[in] vma 要映射的 VMA（vm_start/vm_end/vm_flag 必须已填写）
 * @retval ENO0_NO_ERROR   成功，区间内所有页均已映射
 * @retval ENO1_NOMORE_MEM 物理内存不足，已映射的部分不会自动回滚
 * @note 每个物理帧分配后清零，PTE 权限由 vma->vm_flag 经 vma_prot_to_pte_flags() 转换得到。
 *   与懒分配相对，此函数适用于需要立即保证物理内存存在的场景（如 fork 子进程复制）。
 */
int vmm_map_vma(mm_t *mm, vma_t *vma)
{
    pteflg_t flags = vma_prot_to_pte_flags(vma->vm_flag);
    for (virAddr_t va = vma->vm_start; va < vma->vm_end; va += PGSIZE)
    {
        pframe_t *frame = slab_alloc_page_retry();
        if (!frame)
        {
            return ENO1_NOMORE_MEM;
        }
        frame->reference++;
        memset((void *)convert_pframe2kva(frame), 0, PGSIZE);
        pte_t *ptep = get_pte(mm->pgd_ppn, va, true, true);
        if (!ptep)
        {
            pmm_free_pages(frame);
            return ENO1_NOMORE_MEM;
        }
        *ptep = pte_create(convert_pframe2ppn(frame), flags);
        tlb_flush_va(va);
    }
    return ENO0_NO_ERROR;
}

/**
 * @brief 把一个已存在的物理帧映到用户地址空间的固定虚拟地址上
 * @param[in] mm   目标地址空间
 * @param[in] va   目标虚拟地址（必须页对齐）
 * @param[in] ppn  要映射的物理页帧号
 * @param[in] prot VMP_R/W/X 组合，转成 PTE 标志时始终带 PTE_U
 * @retval ENO0_NO_ERROR   成功
 * @retval ENO1_NOMORE_MEM 建中间页表时物理内存不足
 * @note 与 vmm_map_vma() 的区别是不分配新帧，用于把内核准备好的共享页
 *   （目前只有 sigpage）塞进每个用户地址空间。
 * @note 与 vmm_map_vma() 一样，每建立一次映射就 reference++：拆除侧按映射逐一递减、
 *   归零才还给 PMM。不加就会"映射了 N 份只记 1 份"，第一个进程退出就把这页还掉，
 *   其余进程的 PTE 指向一页随时会被别人拿走的内存。
 */
int vmm_map_fixed_page(mm_t *mm, virAddr_t va, ppn_t ppn, pgprot_t prot)
{
    pte_t *ptep = get_pte(mm->pgd_ppn, va, true, true);
    if (!ptep)
    {
        return ENO1_NOMORE_MEM;
    }
    convert_ppn2pframe(ppn)->reference++;
    *ptep = pte_create(ppn, vma_prot_to_pte_flags(prot));
    tlb_flush_va(va);
    return ENO0_NO_ERROR;
}

/**
 * @brief 解除 VMA 描述的整个虚拟地址区间的物理页映射并归还物理帧
 * @param[in] mm  所属进程地址空间描述符
 * @param[in] vma 要解映射的 VMA
 * @note 此函数不释放页表中间节点帧，也不释放 vma_t 描述符本身；
 *       调用者需另行处理（见 vmm_mm_destroy()）。
 */
void vmm_unmap_vma(mm_t *mm, vma_t *vma)
{
    vmm_unmap_range(mm, vma->vm_start, vma->vm_end);
}

/**
 * @brief 解除 [start, end) 范围内的映射并按引用计数回收物理帧
 * @param[in] mm    目标地址空间
 * @param[in] start 起始虚拟地址（页对齐，含）
 * @param[in] end   结束虚拟地址（页对齐，不含）
 * @details 逐页查 PTE：无效则跳过；否则递减 pframe_t.reference，归零时 pmm_free_pages()，
 *   随后清零 PTE 并刷新该地址的 TLB。
 * @note 不释放页表中间节点帧，也不动 mm->mmap_list 上的 vma_t——
 *   VMA 的删除/截断/分裂由调用方负责。
 */
void vmm_unmap_range(mm_t *mm, virAddr_t start, virAddr_t end)
{
    for (virAddr_t va = start; va < end; va += PGSIZE)
    {
        pte_t *ptep = get_pte(mm->pgd_ppn, va, false, true);
        if (!ptep || !pte_is_valid(*ptep))
        {
            continue;
        }
        ppn_t ppn = (*ptep) >> PTE_PPN_OFFSET;
        pframe_t *frame = convert_ppn2pframe(ppn);

        /* 同一帧可能还被别的进程共享，其它 hart 上的 fork/page fault 可能正
         * 同时改它的 reference，减计数+判断归零必须整体互斥。 */
        irq_key_t vmm_lock_key = spinlock_acquire(&vmm_lock);
        frame->reference--;
        if (frame->reference == 0)
        {
            pmm_free_pages(frame);
        }
        spinlock_release(&vmm_lock, vmm_lock_key);

        *ptep = 0;
        tlb_flush_va(va);
    }
}

/* 页表帧的引用计数由 get_pte() 建表时 +1，这里对称地减回去。 */
static void free_table_frame(ppn_t ppn)
{
    pframe_t *frame = convert_ppn2pframe(ppn);
    frame->reference--;
    if (frame->reference == 0)
    {
        pmm_free_pages(frame);
    }
}

/**
 * @brief 回收用户地址空间自己的页表帧：中间级与根 PGD
 * @param[in] mm 目标地址空间
 * @details 用户虚拟地址全部在 USER_STACK_TOP（1 GB）以下，在 Sv39 里只占 PGD[0] 一项，
 *   所以只遍历低半段。PGD[256..511] 是从内核页表复制来的，指向的是共享的内核中间级表，
 *   跟着释放会把内核页表拆掉。
 * @note 只在叶子映射已由 vmm_unmap_vma() 解完之后调用。共享内核页表的 mm 没有自己的
 *   页表，直接返回。这些帧只能经本 mm 的页表到达，而调用时进程已不再运行，故不取 vmm_lock。
 */
static void free_user_page_table(mm_t *mm)
{
    if (mm->pgd_ppn == vmm_kernel_pgd_ppn)
    {
        return;
    }

    pte_t *pgd = (pte_t *)pa_to_kva(convert_ppn2pa(mm->pgd_ppn));
    for (uint64_t i = 0; i < PGD_KERNEL_START; i++)
    {
        if (!pte_is_valid(pgd[i]) || pte_is_readable(pgd[i]) ||
            pte_is_writable(pgd[i]) || pte_is_executable(pgd[i]))
        {
            continue; /* 空项，或本身就是大页叶子——都没有下一级可回收 */
        }

        ppn_t pmd_ppn = pgd[i] >> PTE_PPN_OFFSET;
        pte_t *pmd = (pte_t *)pa_to_kva(convert_ppn2pa(pmd_ppn));
        for (uint64_t j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            if (!pte_is_valid(pmd[j]) || pte_is_readable(pmd[j]) ||
                pte_is_writable(pmd[j]) || pte_is_executable(pmd[j]))
            {
                continue;
            }
            free_table_frame(pmd[j] >> PTE_PPN_OFFSET);
        }
        free_table_frame(pmd_ppn);
        pgd[i] = 0;
    }
    free_table_frame(mm->pgd_ppn);
}

/**
 * @brief 销毁进程地址空间：解映射所有 VMA，回收页表帧，释放 mm_t 本身
 * @param[in] mm 要销毁的进程地址空间描述符
 * @details 遍历 mmap_list，对每个 VMA 依次调用 vmm_unmap_vma() 和 vmm_vma_destroy()，
 *   再由 free_user_page_table() 回收该地址空间自己的页表帧，最后 kfree(mm)。
 * @note 调用前必须确保进程已不再运行，且当前 hart 已切回内核页表——正在用的页表不能拆。
 */
void vmm_mm_destroy(mm_t *mm)
{
    struct list_head *tmp, *cur;
    list_for_each_safe(cur, tmp, &mm->mmap_list)
    {
        vma_t *vma = list_entry(cur, vma_t, vma_list_linker);
        vmm_unmap_vma(mm, vma);
        list_del(cur);
        vmm_vma_destroy(vma);
    }
    free_user_page_table(mm);
    kfree(mm);
}

/**
 * @brief 处理一次非法访问：U 态发起的只杀该进程，S 态发起的 panic
 * @param[in] badva 触发异常的虚拟地址
 * @param[in] why   诊断用的原因描述
 * @details sstatus.SPP 是进入本次 trap 之前的特权级，中间没有嵌套 trap，所以它就是
 *   "谁踩的这一下"。
 * @note 本函数不返回：U 态走 do_exit_signal(SIGSEGV)，S 态 panic。
 */
static void vmm_segfault(virAddr_t badva, const char *why)
{
    pcb_t *curr = proc_get_current();
    /* sepc 一起打出来：S 态故障时它是唯一能把现场对回到具体调用点的线索
     * （反汇编 build/kernel.elf 查这个地址即可），偶发故障没有第二次机会 */
    printf("vmm: segfault - %s va=0x%lx sepc=0x%lx spp=%d pid=%d\n",
           why, badva, read_csr(sepc),
           (read_csr(sstatus) & SSTATUS_SPP) ? 1 : 0, curr->proc_pid);
    if ((read_csr(sstatus) & SSTATUS_SPP) == 0)
    {
        printf("vmm: killing pid=%d\n", curr->proc_pid);
        /* 走信号的退出路径，父进程 wait 到的 status 低 7 位才会是 SIGSEGV(11)；
         * 从前写死的 do_exit(139) 是"退出码 139"，WIFSIGNALED 判不出来 */
        do_exit_signal(SIGSEGV);
    }
    panic("segfault");
}

/**
 * @brief 处理用户空间页错误（懒分配 + 写时复制拆分）
 * @param[in] badva      触发页错误的虚拟地址（来自 stval/sbadaddr）
 * @param[in] fault_type 0 = 取指，1 = 读，2 = 写
 * @details 找不到 VMA 或权限不符 → vmm_segfault()（U 态杀进程，S 态 panic）；
 *   写故障且 PTE 已有效 → COW 拆分：reference == 1 原地补回可写位，否则复制新帧；
 *   其余情况按懒分配建新页。最后统一 tlb_flush_va()。
 * @note 由 trap.c 的 trap_dispatch() 调用，运行在关中断的 trap 上下文里。
 */
void vmm_page_fault_handler(virAddr_t badva, int fault_type)
{
    pcb_t *curr = proc_get_current();
    /* curr 为 NULL 只应发生在本 hart 尚未跑到 proc_init() 的极早期窗口——真正的缺页
     * 不该落到这里，落到这里说明有别的地方在 current_proc 建立之前触发了一次访存异常。
     * 不加这层判断的话，NULL->proc_mm 会在处理这次异常的过程中再触发一次异常，
     * 陷入嵌套故障；显式判断能把它变成一条可诊断的 panic，而不是静默死循环。 */
    if (!curr)
    {
        printf("vmm: page fault with no current proc (va=0x%lx)\n", badva);
        panic("kernel page fault");
    }

    mm_t *mm = curr->proc_mm;
    if (!mm)
    {
        printf("vmm: page fault with no mm (va=0x%lx)\n", badva);
        panic("kernel page fault");
    }

    vma_t *vma = vmm_vma_get(mm, badva);
    if (!vma)
    {
        vmm_segfault(badva, "no vma");
    }

    if (fault_type == 2 && !(vma->vm_flag & VMP_W))
    {
        vmm_segfault(badva, "write on non-writable vma");
    }
    if (fault_type == 0 && !(vma->vm_flag & VMP_X))
    {
        vmm_segfault(badva, "exec on non-exec vma");
    }

    virAddr_t page_va = badva & ~(PGSIZE - 1);
    pteflg_t  flags   = vma_prot_to_pte_flags(vma->vm_flag);

    if (fault_type == 2)
    {
        pte_t *ptep = get_pte(mm->pgd_ppn, page_va, false, true);
        if (ptep && pte_is_valid(*ptep))
        {
            /* reference 的读-判断-改必须整体互斥：另一个共享此帧的进程可能正在
             * 别的 hart 上对同一个 pframe_t 做同样的事。 */
            irq_key_t vmm_lock_key = spinlock_acquire(&vmm_lock);

            ppn_t old_ppn = (*ptep) >> PTE_PPN_OFFSET;
            pframe_t *old_frame = convert_ppn2pframe(old_ppn);

            if (old_frame->reference == 1)
            {
#if DEBUG_COW
                printf("vmm: cow in-place va=0x%lx\n", page_va);
#endif
                *ptep = pte_create(old_ppn, flags);
            }
            else
            {
#if DEBUG_COW
                printf("vmm: cow duplicate va=0x%lx refs=%u\n", page_va, old_frame->reference);
#endif
                pframe_t *new_frame = slab_alloc_page_retry();
                if (!new_frame)
                {
                    panic("vmm: OOM in COW fault handler");
                }
                new_frame->reference++;
                memcpy((void *)convert_pframe2kva(new_frame),
                       (void *)pa_to_kva(convert_ppn2pa(old_ppn)), PGSIZE);
                old_frame->reference--;
                *ptep = pte_create(convert_pframe2ppn(new_frame), flags);
            }

            spinlock_release(&vmm_lock, vmm_lock_key);
            tlb_flush_va(page_va);
            return;
        }
    }

    /* 该 va 从未被映射过：懒分配一个清零页。
     * 分配与清零放锁外（slab_alloc_page_retry 失败会走 slab_reclaim_all，不该拖进 vmm_lock），
     * get_pte 建中间级 + 装 PTE 放锁内（两个执行流同时缺页会各建一份中间级、后者覆盖前者）。
     * 拿到锁后要重看一眼 PTE：别的执行流可能已经在这空档里装好了，这时放掉自己那页用它的，
     * 否则先装的那页被覆盖、永久泄漏。 */
    pframe_t *frame = slab_alloc_page_retry();
    if (!frame)
    {
        panic("vmm: OOM in page fault handler");
    }
    memset((void *)convert_pframe2kva(frame), 0, PGSIZE);

    irq_key_t vmm_lock_key = spinlock_acquire(&vmm_lock);

    pte_t *ptep = get_pte(mm->pgd_ppn, page_va, true, true);
    if (!ptep)
    {
        spinlock_release(&vmm_lock, vmm_lock_key);
        pmm_free_pages(frame);
        panic("vmm: get_pte failed in page fault");
    }
    if (pte_is_valid(*ptep))
    {
        spinlock_release(&vmm_lock, vmm_lock_key);
        pmm_free_pages(frame);
        tlb_flush_va(page_va);
        return;
    }

    frame->reference++;
    *ptep = pte_create(convert_pframe2ppn(frame), flags);

    spinlock_release(&vmm_lock, vmm_lock_key);
    tlb_flush_va(page_va);
}

/**
 * @brief 将源进程地址空间的所有 VMA 共享给目标地址空间（写时复制，用于 fork）
 * @param[in,out] dst 目标 mm_t（已由 vmm_mm_create() 初始化，mmap_list 为空）
 * @param[in]     src 源 mm_t
 * @retval ENO0_NO_ERROR   成功，dst 拥有与 src 相同布局、共享同一批物理帧的地址空间
 * @retval ENO1_NOMORE_MEM 物理内存不足（仅可能发生在 dst 侧页表中间节点分配失败）；
 *   已处理的部分不会自动回滚——src 侧已被降权的 PTE 会在下次该进程自己写入时于
 *   page fault handler 里发现 reference==1，原地补回可写位，不会造成数据损坏。
 * @details 对 src 的每个 VMA：
 *   1. 创建相同范围和权限的新 vma_t 插入 dst；
 *   2. 遍历区间内每个已映射页（PTE 有效），不分配新帧、不 memcpy：
 *      递增该物理帧的 reference，src 与 dst 的 PTE 都清除 PTE_W 指向同一帧；
 *   3. src 的 PTE 是父进程正在使用的页表条目，降权后必须 tlb_flush_va()，
 *      否则父进程可能凭 TLB 里缓存的旧"可写"翻译绕过缺页异常，直接写坏
 *      与子进程共享的物理页。
 *
 *   未映射的页（懒分配尚未触发的页）不处理，子进程首次访问时触发页错误再分配。
 *   写时复制的实际"拆分共享页"逻辑在 vmm_page_fault_handler() 里完成。
 * @note brk_start 和 brk_current 也一并复制。
 */
int vmm_mm_copy(mm_t *dst, mm_t *src)
{
    struct list_head *cur;
    list_for_each(cur, &src->mmap_list)
    {
        vma_t *sv = list_entry(cur, vma_t, vma_list_linker);
        vma_t *dv = vmm_vma_create(sv->vm_start, sv->vm_end, sv->vm_flag);
        if (!dv)
        {
            return ENO1_NOMORE_MEM;
        }
        vmm_vma_insert(dst, dv);
        for (virAddr_t va = sv->vm_start; va < sv->vm_end; va += PGSIZE)
        {
            pte_t *sp = get_pte(src->pgd_ppn, va, false, true);
            if (!sp || !pte_is_valid(*sp))
            {
                continue;
            }

            ppn_t ppn = (*sp) >> PTE_PPN_OFFSET;
            pframe_t *frame = convert_ppn2pframe(ppn);

            /* frame->reference 可能同时被这个帧的另一个共享者在别的 hart 上
             * 改（page fault 拆分 / 进程退出释放），整段增计数+改 PTE 得互斥。 */
            irq_key_t vmm_lock_key = spinlock_acquire(&vmm_lock);
            frame->reference++;

            /* 双方共享同一帧，都改成只读：谁先写谁在 page fault 里触发拆分 */
            pteflg_t ro_flags = pte_get_flag(*sp) & ~(pteflg_t)PTE_W;
            *sp = pte_create(ppn, ro_flags);
            tlb_flush_va(va);

            pte_t *dp = get_pte(dst->pgd_ppn, va, true, true);
            if (!dp)
            {
                frame->reference--;
                spinlock_release(&vmm_lock, vmm_lock_key);
                return ENO1_NOMORE_MEM;
            }
            *dp = pte_create(ppn, ro_flags); /* 使用sp的ppn，实现共享 */
            spinlock_release(&vmm_lock, vmm_lock_key);
        }
    }
    dst->brk_start   = src->brk_start;
    dst->brk_current = src->brk_current;
    return ENO0_NO_ERROR;
}

#if DEBUG_BRINGUP
/**
 * @brief 打印 MMU 开启所依赖的两条关键映射
 * @details satp 一写，PC 先靠 trampoline 的恒等映射在低地址执行，再跳到高 VA 的 _start_virtual。
 *   这两条 PTE 任何一条不对，表现都是"写完 satp 就没声了"：那时 stvec 还是 0，连 panic 都吐不出来。
 * @note 必须在 MMU 开启之前调用（get_pte 的 mmu_enabled 传 false）。
 */
void vmm_dump_boot_mappings(void)
{
    printf("mmu: pgd_ppn=0x%lx, satp will be 0x%lx\n",
           (unsigned long)vmm_kernel_pgd_ppn,
           (unsigned long)(0x8000000000000000UL | vmm_kernel_pgd_ppn));

    /* 恒等映射：VA 就是 trampoline 的物理地址（MMU 未开，符号取到的即 PA） */
    virAddr_t tramp_va = (virAddr_t)_trampoline_start;
    pte_t *pte = get_pte(vmm_kernel_pgd_ppn, tramp_va, false, false);
    printf("mmu: tramp  va=0x%lx pte=0x%lx\n",
           (unsigned long)tramp_va, (unsigned long)(pte != NULL ? *pte : 0));

    /* 高 VA 映射：trampoline 末尾 jr 过去的目标 */
    virAddr_t sv_va = pa_to_kva((phyAddr_t)_start_virtual);
    pte = get_pte(vmm_kernel_pgd_ppn, sv_va, false, false);
    printf("mmu: startv va=0x%lx pte=0x%lx\n",
           (unsigned long)sv_va, (unsigned long)(pte != NULL ? *pte : 0));
}
#endif

#if DEBUG_PTE_AD_PROBE
/**
 * @brief 实测本平台是否由硬件自动置位 PTE 的 A（访问）/ D（脏）标志
 * @details 规范允许两种实现：硬件在页表遍历时自动写回 A/D，或硬件不写、访问 A=0 的页时
 *   抛缺页交由软件置位。时钟置换算法完全依赖前者，所以要实测而不能照规范假设。
 *   取一个刚分配的物理页，依次观察四个时刻的 A/D：分配后、手工清零并刷 TLB 后、读后、写后。
 * @note 若本平台是软件管理 A 位，第三步的读访问会直接触发缺页异常——
 *   打印顺序已保证在那之前能看到前两条输出，据此即可判断。
 */
void vmm_probe_pte_ad(void)
{
    pframe_t *frame = pmm_alloc_page();
    if (!frame)
    {
        printf("pte_ad_probe: pmm_alloc_page failed\n");
        return;
    }

    virAddr_t kva = convert_pframe2kva(frame);
    pte_t *ptep = get_pte(vmm_kernel_pgd_ppn, kva, false, true);
    if (!ptep || !pte_is_valid(*ptep))
    {
        printf("pte_ad_probe: no valid pte for kva=0x%lx\n", kva);
        pmm_free_pages(frame);
        return;
    }

    printf("pte_ad_probe: kva=0x%lx pte=0x%lx\n", kva, *ptep);
    printf("pte_ad_probe: [1] after pmm_alloc_pages(memset)  A=%d D=%d\n",
           (*ptep & PTE_A) ? 1 : 0, (*ptep & PTE_D) ? 1 : 0);

    *ptep &= ~(pte_t)(PTE_A | PTE_D);
    tlb_flush_va(kva);
    printf("pte_ad_probe: [2] after manual clear   A=%d D=%d\n",
           (*ptep & PTE_A) ? 1 : 0, (*ptep & PTE_D) ? 1 : 0);

    /* 不碰这一页，只做无关工作，确认 A 不会被无端置回——时钟算法要靠
     * "清零之后还是 0" 来判断这一轮没被访问过，这条不成立算法就失效。 */
    for (volatile int i = 0; i < 1000; i++)
    {
    }
    printf("pte_ad_probe: [2b] after unrelated work A=%d D=%d (expect 0 0)\n",
           (*ptep & PTE_A) ? 1 : 0, (*ptep & PTE_D) ? 1 : 0);

    printf("pte_ad_probe: issuing LOAD...\n");
    volatile uint8_t got = *(volatile uint8_t *)kva;
    (void)got;
    printf("pte_ad_probe: [3] after LOAD           A=%d D=%d\n",
           (*ptep & PTE_A) ? 1 : 0, (*ptep & PTE_D) ? 1 : 0);

    printf("pte_ad_probe: issuing STORE...\n");
    *(volatile uint8_t *)kva = 0x5a;
    printf("pte_ad_probe: [4] after STORE          A=%d D=%d\n",
           (*ptep & PTE_A) ? 1 : 0, (*ptep & PTE_D) ? 1 : 0);

    /* 关键一问：清 A 位而不刷 TLB 时，后续访问还会不会重走页表把 A 写回内存。
     * 若不会（TLB 命中直接跳过页表遍历），时钟算法每清一个候选页的 A 位就必须
     * 配一次 tlb_flush_va，否则会把刚被访问过的热页误判成冷页换出去。 */
    *ptep &= ~(pte_t)PTE_A;
    /* 故意不刷 TLB */
    volatile uint8_t again = *(volatile uint8_t *)kva;
    (void)again;
    int a_without_sfence = (*ptep & PTE_A) ? 1 : 0;
    printf("pte_ad_probe: [5] LOAD after clear w/o sfence  A=%d\n", a_without_sfence);

    if ((*ptep & PTE_A) || (*ptep & PTE_D))
    {
        printf("pte_ad_probe: RESULT = hardware updates A/D -> clock usable%s\n",
               a_without_sfence ? "" : " (MUST sfence.vma after clearing A)");
    }
    else
    {
        printf("pte_ad_probe: RESULT = A/D NOT updated by hardware -> clock degrades to fifo\n");
    }

    *ptep |= (pte_t)(PTE_A | PTE_D);
    tlb_flush_va(kva);
    pmm_free_pages(frame);
}
#endif
