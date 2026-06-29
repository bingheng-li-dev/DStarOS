#include "vmm.h"
#include "errorcode.h"
#include "kmalloc.h"
#include "console.h"
#include "proc.h"
#include "stringops.h"

/**
 * @brief 内核页表根目录的物理页号。
 * @details 一个SV39页表项占64位，因此一页（4096字节）内存中内核页表项数组大小为512
 * [0, 255]，对应虚拟地址[0, 0x0000000000000000 ~ 0x0000003FFFFFFFFF]，被用户空间使用；
 * [256, 511]，对应虚拟地址[0xFFFFFFC000000000 ~ 0xFFFFFFFFFFFFFFFF]，被内核高位映射使用。
 */
ppn_t vmm_kernel_pgd_ppn;

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

    /* 找一级页表项，pgd是一定存在的，其在进程创建时分配了内存空间 */
    phyAddr_t pgd_pa = convert_ppn2pa(pgd_ppn); /* pgd_pa代表一级页表的物理地址 */
    pte_t *pgd = mmu_enabled ? (pte_t *)pa_to_kva(pgd_pa) : (pte_t *)pgd_pa;
    if (!pte_is_valid(pgd[pgd_idx]))
    {
        if (!create)
        {
            return NULL; /* 如果不创建新页表项，直接返回 NULL */
        }

        pframe_t *new_pmd_frame = alloc_page();
        if (!new_pmd_frame)
        {
            return NULL;
        }
        new_pmd_frame->reference += 1;
        pgd[pgd_idx] = pte_create(convert_pframe2ppn(new_pmd_frame), 0);
    }

    /* 找二级页表项 */
    phyAddr_t pmd_pa = convert_ppn2pa(pgd[pgd_idx] >> PTE_PPN_OFFSET);
    pte_t *pmd = mmu_enabled ? (pte_t *)pa_to_kva(pmd_pa) : (pte_t *)pmd_pa;
    if (!pte_is_valid(pmd[pmd_idx]))
    {
        if (!create)
        {
            return NULL;
        }

        pframe_t *newPte_frame = alloc_page();
        if (!newPte_frame)
        {
            return NULL;
        }
        newPte_frame->reference += 1;
        pmd[pmd_idx] = pte_create(convert_pframe2ppn(newPte_frame), 0);
    }

    phyAddr_t pte_pa = convert_ppn2pa(pmd[pmd_idx] >> PTE_PPN_OFFSET);
    pte_t *pte = mmu_enabled ? (pte_t *)pa_to_kva(pte_pa) : (pte_t *)pte_pa;

    /* 返回最终的页表项指针，不存在也不需要alloc */
    if (!create && !pte_is_valid(pte[pte_idx]))
    {
        return NULL;
    }
    return &pte[pte_idx];   /* 当create为true时，总是返回页表项地址，即使不存在 */
}

/**
 * @brief 建立内核高位偏移映射（Offset Mapping），将全部物理内存线性映射到高虚拟地址
 * @details 映射关系为 VA = PA + KERNEL_VA_OFFSET（0xffffffc000000000）。
 *   按内核链接脚本的分段结构分配不同 PTE 权限：
 *   - .text 段：PTE_G | PTE_R | PTE_X
 *   - .rodata 段：PTE_G | PTE_R
 *   - .data/.bss 段及剩余物理内存：PTE_G | PTE_R | PTE_W
 *
 *   此外还为 trampoline 段建立临时恒等映射（VA = PA），
 *   使 enable_mmu 之后 PC 仍在低地址时能被 MMU 正常翻译。
 *   恒等映射在 vmm_remove_identity_mapping() 中被撤销。
 * @note 此函数在 MMU 尚未启用时调用，所有页表帧访问均使用物理地址作为指针
 *       （get_pte 的 mmu_enabled 参数传 false）。
 */
static void init_kernel_offset_mapping(void)
{
    pframe_t *kernel_pgd_pframe = alloc_page();
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
    ppn_t ppn_end      = convert_pa2ppn_cil(MEMORY_END);

    /** 偏移映射：VA = PA + KERNEL_VA_OFFSET → PA，按段分配权限
     * 由ld文件，内存布局为skernel → text → rodata → data → bss → ekernel → 其他物理内存
     */
    for (ppn_t ppn = ppn_skernel; ppn < ppn_etext; ppn++)
    {
        /* 创建页表，根据内核虚拟地址创建对应的pgd，pmd以及pte */
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, pa_to_kva(convert_ppn2pa(ppn)), true, false);
        /* 内核代码段可读可执行 */
        *pte = pte_create(ppn, PTE_G | PTE_R | PTE_X); /* PTE_G表示这个映射是否对所有虚址空间有效 */
    }
    for (ppn_t ppn = ppn_etext; ppn < ppn_erodata; ppn++)
    {
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, pa_to_kva(convert_ppn2pa(ppn)), true, false);
        /* 内核只读数据段只读 */
        *pte = pte_create(ppn, PTE_G | PTE_R);
    }
    /** 从data段开始，包括bss段权限都是可读可写
     * 值得注意的是，直到物理内存结尾而非内核结尾的部分全部加入了内核页表，并且除了RW权限外还设置了G标志位
     * 赋予PTE_G标志位的原因如下：
     * PTE_G（Global 位）的语义：该 PTE 对所有 ASID 有效，
     * sfence.vma rs1, asid（按 ASID 刷 TLB）不会清除 G 位的条目，只有 sfence.vma zero, zero 才清。
     * 通过将全部内存永久映射到高 VA 区，让内核随时可以用 pa_to_kva(pa) 访问任意物理地址
     *
     * 而内核以外的物理帧被分配给用户进程后会有两个映射：
     * 内核页表有一个，内核随时可以通过 KVA 读写用户物理帧（例如 copy-on-write 时复制内容）
     * 用户页表有一个，这个是用户进程自己的映射
     *
     * 如果DStarOS后续没有实现ASID（所有进程用 ASID 0 或不区分），sfence.vma 都是全刷，PTE_G 现在既无害也无益
     * 若后续实现了 ASID 切换，G 位就能真正减少 TLB miss
     */
    for (ppn_t ppn = ppn_erodata; ppn < ppn_end; ppn++)
    {
        pte_t *pte = get_pte(vmm_kernel_pgd_ppn, pa_to_kva(convert_ppn2pa(ppn)), true, false);
        /* 内核数据段可读可写 */
        *pte = pte_create(ppn, PTE_G | PTE_R | PTE_W);
    }

    /**
     * 临时恒等映射：仅覆盖 trampoline （见ld）代码所在页（使得VA = PA）
     * enable_mmu 后 PC 还在低地址，需要这几页能被 MMU 翻译
     *
     * 注意：此函数在 MMU 关闭时调用，auipc 相对寻址的结果是物理地址而非虚拟地址
     * （offset = VMA目标 - VMA指令，runtime结果 = PA指令 + offset = PA目标）
     * 因此不能再套 kva_to_pa()，那会再减一次 KERNEL_VA_OFFSET 得到错误值。
     * 对比 vmm_remove_identity_mapping()：那里在 MMU 已启用后调用，拿到的才是 VA，kva_to_pa 才正确。
     */
    extern char _trampoline_start[], _trampoline_end[];
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
 * @brief 撤销内核页表中 trampoline 段的临时恒等映射（VA = PA）
 * @details MMU 启用后，PC 已跳转到高虚拟地址，trampoline 代码不再需要恒等映射。
 *   此函数逐页清除对应 PTE（置 0，即清除 PTE_V），并执行全局 TLB 刷新。
 * @note 必须在 MMU 启用、内核已运行在高 VA 之后调用；
 *       调用后任何通过低地址访问 trampoline 的操作都将触发页错误。
 */
void vmm_remove_identity_mapping(void)
{
    extern char _trampoline_start[], _trampoline_end[];
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
    init_kernel_offset_mapping();
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
 * @note 新建的 mm_t 共享内核页表（pgd_ppn = vmm_kernel_pgd_ppn）。
 *   用户进程独立页表阶段需在此处分配新 PGD 并复制内核半段（PGD[256..511]）。
 */
mm_t *vmm_mm_create(void)
{
    mm_t *ret = kmalloc(sizeof(mm_t));
    if (ret != NULL)
    {
        ret->last_access = NULL;
        ret->map_count   = 0;
        ret->pgd_ppn     = vmm_kernel_pgd_ppn;
        ret->brk_start   = 0;
        ret->brk_current = 0;
        INIT_LIST_HEAD(&ret->mmap_list);
    }
    return ret;
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
    vma_t *ret = kmalloc(sizeof(vma_t));
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
            /* 插入到 cur 之前，保持 vm_start 升序 */
            list_add_tail(&vma->vma_list_linker, pos);
            mm->map_count++;
            return;
        }
    }
    list_add_tail(&vma->vma_list_linker, &mm->mmap_list);
    mm->map_count++;
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
        pframe_t *frame = alloc_page();
        if (!frame)
        {
            return ENO1_NOMORE_MEM;
        }
        frame->reference++;
        memset((void *)convert_pframe2kva(frame), 0, PGSIZE);
        pte_t *ptep = get_pte(mm->pgd_ppn, va, true, true);
        if (!ptep)
        {
            dealloc(frame);
            return ENO1_NOMORE_MEM;
        }
        *ptep = pte_create(convert_pframe2ppn(frame), flags);
        tlb_flush_va(va);
    }
    return ENO0_NO_ERROR;
}

/**
 * @brief 解除 VMA 描述的整个虚拟地址区间的物理页映射并归还物理帧
 * @param[in] mm  所属进程地址空间描述符
 * @param[in] vma 要解映射的 VMA
 * @details 对区间内每个页：
 *   1. 查找对应 PTE，若无效则跳过；
 *   2. 递减物理帧引用计数，计数归零时调用 dealloc() 释放帧；
 *   3. 将 PTE 清零（清除 PTE_V）并刷新 TLB。
 * @note 此函数不释放页表中间节点帧，也不释放 vma_t 描述符本身；
 *       调用者需另行处理（见 vmm_mm_destroy()）。
 */
void vmm_unmap_vma(mm_t *mm, vma_t *vma)
{
    for (virAddr_t va = vma->vm_start; va < vma->vm_end; va += PGSIZE)
    {
        pte_t *ptep = get_pte(mm->pgd_ppn, va, false, true);
        if (!ptep || !pte_is_valid(*ptep))
        {
            continue;
        }
        ppn_t ppn = (*ptep) >> PTE_PPN_OFFSET;
        pframe_t *frame = convert_ppn2pframe(ppn);
        frame->reference--;
        if (frame->reference == 0)
        {
            dealloc(frame);
        }
        *ptep = 0;
        tlb_flush_va(va);
    }
}

/**
 * @brief 销毁进程地址空间：解映射所有 VMA，释放物理帧，释放 mm_t 本身
 * @param[in] mm 要销毁的进程地址空间描述符
 * @details 遍历 mmap_list，对每个 VMA 依次调用 vmm_unmap_vma() 和 vmm_vma_destroy()，
 *   最后 kfree(mm)。页表中间节点帧（PMD/PTE 帧）在当前阶段不单独释放；
 *   待引入独立用户页表后，应在此处调用 free_page_table() 回收中间节点。
 * @note 调用前必须确保进程已不再运行，否则另一 hart 可能仍在使用被释放的帧。
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
    kfree(mm);
}

/**
 * @brief 处理用户空间页错误（懒分配实现）
 * @param[in] badva      触发页错误的虚拟地址（来自 stval/sbadaddr 寄存器）
 * @param[in] fault_type 错误类型：0 = 指令取指页错误，1 = 读页错误，2 = 写页错误
 * @details 处理流程：
 *   1. 从当前进程（TaskCurrent）获取 mm_t；内核线程的 mm 为 NULL，视为内核页错误直接 panic。
 *   2. 通过 vmm_vma_get() 查找包含 badva 的 VMA；未找到表示非法访问，panic（segfault）。
 *   3. 权限检查：写操作要求 VMP_W，取指要求 VMP_X；不满足则 panic（segfault）。
 *   4. 分配新物理帧，清零，建立 PTE，刷新 TLB。
 * @note 此函数由 trap.c 中的 kernelTrapHandler() 调用，运行在中断上下文中（中断已关闭）。
 *   panic 路径不会返回；正常路径返回后，异常指令将被重新执行。
 */
void vmm_page_fault_handler(virAddr_t badva, int fault_type)
{
    extern pcb_t *TaskCurrent;
    mm_t *mm = TaskCurrent->proc_mm;
    if (!mm)
    {
        printf("vmm: page fault with no mm (va=0x%lx)\n", badva);
        panic("kernel page fault");
    }

    vma_t *vma = vmm_vma_get(mm, badva);
    if (!vma)
    {
        printf("vmm: segfault - no vma for va=0x%lx\n", badva);
        panic("segfault");
    }

    if (fault_type == 2 && !(vma->vm_flag & VMP_W))
    {
        printf("vmm: write fault on non-writable vma va=0x%lx\n", badva);
        panic("segfault");
    }
    if (fault_type == 0 && !(vma->vm_flag & VMP_X))
    {
        printf("vmm: exec fault on non-exec vma va=0x%lx\n", badva);
        panic("segfault");
    }

    pframe_t *frame = alloc_page();
    if (!frame)
    {
        panic("vmm: OOM in page fault handler");
    }
    frame->reference++;
    memset((void *)convert_pframe2kva(frame), 0, PGSIZE);

    /* 将触发页错误的虚拟地址对齐到页面边界（低12位即页内偏移清零） */
    virAddr_t page_va = badva & ~(PGSIZE - 1);
    pteflg_t  flags   = vma_prot_to_pte_flags(vma->vm_flag);
    pte_t    *ptep    = get_pte(mm->pgd_ppn, page_va, true, true);
    if (!ptep)
    {
        dealloc(frame);
        panic("vmm: get_pte failed in page fault");
    }
    *ptep = pte_create(convert_pframe2ppn(frame), flags);
    tlb_flush_va(page_va);
}

/**
 * @brief 将源进程地址空间的所有 VMA 及物理页内容深拷贝到目标地址空间（用于 fork）
 * @param[in,out] dst 目标 mm_t（已由 vmm_mm_create() 初始化，mmap_list 为空）
 * @param[in]     src 源 mm_t
 * @retval ENO0_NO_ERROR   成功，dst 拥有与 src 相同布局的独立物理页副本
 * @retval ENO1_NOMORE_MEM 物理内存不足；已复制的部分不会自动回滚
 * @details 对 src 的每个 VMA：
 *   1. 创建相同范围和权限的新 vma_t 插入 dst；
 *   2. 遍历区间内每个已映射页（PTE 有效），分配新帧并 memcpy 内容；
 *   3. 在 dst 页表中建立新帧的 PTE。
 *
 *   未映射的页（懒分配尚未触发的页）不复制，子进程首次访问时触发页错误再分配。
 * @note 当前为 Eager Copy（写时复制 COW 留待后续实现）。
 *   brk_start 和 brk_current 也一并复制。
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

            pframe_t *df = alloc_page();
            if (!df)
            {
                return ENO1_NOMORE_MEM;
            }
            df->reference++;
            
            ppn_t src_ppn = (*sp) >> PTE_PPN_OFFSET;
            /* 把源进程的一个物理页内容拷贝到子进程新分配的物理页 */
            memcpy((void *)convert_pframe2kva(df),
                   (void *)pa_to_kva(convert_ppn2pa(src_ppn)), PGSIZE);
            pteflg_t  flags = vma_prot_to_pte_flags(sv->vm_flag);
            pte_t    *dp    = get_pte(dst->pgd_ppn, va, true, true);
            if (!dp)
            {
                dealloc(df);
                return ENO1_NOMORE_MEM;
            }
            *dp = pte_create(convert_pframe2ppn(df), flags);
        }
    }
    dst->brk_start   = src->brk_start;
    dst->brk_current = src->brk_current;
    return ENO0_NO_ERROR;
}
