#include "vmm.h"

/* Must be called first before using functions about page table. */
static void kernelPa2Va_IdentityMapping(void);
static void enable_mmu(void);
static inline void pteChangeppn(pte_t *pte, ppn_t ppn);

void vmm_init(void)
{
    kernelPa2Va_IdentityMapping();
    enable_mmu();
    printf("vmm inited!\n");
}

void vmm_pageFaultHander(virAddr_t va)
{
}

static void kernelPa2Va_IdentityMapping(void)
{
    KernelLevel3PageTableFrame = pmm_allocOneFrame(); /* Used for level3 page table. */
    KernelLevel3PageTableFrame->reference = KernelLevel3PageTableFrame->reference + 1;

#if DEBUG_MMU_kernelPa2Va_IdentityMapping
    printf("KernelLevel3PageTableFrame ppn:%ld,pa:%08lx\n", convert_pframe2ppn(KernelLevel3PageTableFrame), convert_pframe2pa(KernelLevel3PageTableFrame));
#endif

    if (KernelLevel3PageTableFrame == (pframe_t *)0)
    {
        printf("NO FREE MEMORY!Must be something wrong...\n");
        while (1)
            ;
    }

    extern unsigned int etext;
    extern unsigned int erodata;
    extern unsigned int edata;
    extern unsigned int ebss;

    ppn_t ppnBase;
    ppn_t ppnEnd;

    ppn_t ppnCursor;
    ppn_t ppn_etext;
    ppn_t ppn_erodata;
    ppn_t ppn_edata;
    ppn_t ppn_ebss;

    pteflg_t kernelPageFlag_text;
    pteflg_t kernelPageFlag_rodata;
    pteflg_t kernelPageFlag_data;
    pteflg_t kernelPageFlag_bss;

    pte_t *cursorPte;
    pframe_t *cursorFrame;
    pframe_t *kernelBasePageTable;

    ppnBase = convert_pa2ppn_flr((phyAddr_t)KERNEL_START);
    ppnEnd = convert_pa2ppn_cil(MEMORY_END);

    ppn_etext = convert_pa2ppn_flr((phyAddr_t)&etext);
    ppn_erodata = convert_pa2ppn_flr((phyAddr_t)&erodata);
    ppn_edata = convert_pa2ppn_flr((phyAddr_t)&edata);
    ppn_ebss = convert_pa2ppn_flr((phyAddr_t)&ebss);

    kernelPageFlag_text = PTE_G | PTE_R | PTE_X;
    kernelPageFlag_rodata = PTE_G | PTE_R;
    kernelPageFlag_data = PTE_G | PTE_R | PTE_W;
    kernelPageFlag_bss = PTE_G | PTE_R | PTE_W;

    kernelBasePageTable = KernelLevel3PageTableFrame;

    for (ppnCursor = ppnBase; ppnCursor <= ppn_etext; ppnCursor++)
    {
        cursorPte = pmm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_text);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        pmm_deallocOneFrame(cursorFrame);
    }

    for (; ppnCursor <= ppn_erodata; ppnCursor++)
    {
        cursorPte = pmm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_rodata);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        pmm_deallocOneFrame(cursorFrame);
    }

    for (; ppnCursor <= ppn_edata; ppnCursor++)
    {
        cursorPte = pmm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_data);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        pmm_deallocOneFrame(cursorFrame);
    }

    for (; ppnCursor <= ppn_ebss; ppnCursor++)
    {
        cursorPte = pmm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_bss);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        pmm_deallocOneFrame(cursorFrame);
    }

    /* All remaining physical memories should be identically mapped to virtual address in order kernel directly accesses physical memories. */
    for (; ppnCursor <= ppnEnd; ppnCursor++)
    {
        cursorPte = pmm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_data);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        pmm_deallocOneFrame(cursorFrame);
    }
}

static void enable_mmu(void)
{
    write_csr(satp, SATPMODE_RV39 | convert_pframe2ppn(KernelLevel3PageTableFrame));
}

static inline void pteChangeppn(pte_t *pte, ppn_t ppn)
{
    *pte = pte_create(ppn, pte_get_flag(*pte));
}
