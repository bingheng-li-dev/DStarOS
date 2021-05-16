#include "mm.h"

pframe_t *PageListBegin;
fslist_t FreeList;  /* Free memories will arrange from small size to large size,used for best fit. */
fslist_t FreeAList; /* Free memories will arrange from small addr to large addr,used for merge. */
pframe_t *KernelLevel3PageTableFrame;
// tt_t PageTableTrieTree;

static pframe_t *deleteAndReinsert(uint16_t nsize);
static void insertAndMerge(pframe_t *baseppn, uint16_t nsize);
/* Must be called first before using functions about page table below. */
static void kernelPa2Va_IdentityMapping(phyAddr_t tKernelEndAddr);
/* Return null if not exists.When using:@param pageTable should be base page table,@param level should be 3. */
static pte_t *searchAndGetPteIfExists(pframe_t *pageTable, virAddr_t va, uint16_t level);
/* Delete the pte which refers to this virtual addr (param va) and dealloc the mapping pframe of this pte if needed. */
static void removePteFromPageTable(virAddr_t va, pte_t *pte);
/* Turn the given va into vpns and insert them into different level page tables. Return null if failed,else return the pte of page table level1. */
static pte_t *insertPteIntoPageTable(pframe_t *pageTable, virAddr_t va, uint16_t level, pteflg_t pteFlag);
static inline void pteChangeppn(pte_t *pte, ppn_t ppn);
static void enable_mmu(void);

const bffa_t BFallocator = {
    .bffa_deleteAndReinsert = deleteAndReinsert,
    .bffa_insertAndMerge = insertAndMerge,
};

void mm_init(void)
{
    extern unsigned int ekernel;
    extern unsigned int skernel;
    extern unsigned int _start;
    phyAddr_t kernelEndAddr = (phyAddr_t)(&ekernel);
    phyAddr_t kernelStartAddr = (phyAddr_t)(&skernel);
    phyAddr_t kernelEntryAddr = (phyAddr_t)(&_start);
    printf("kernel: skernel:0x%08lx, ekernel:0x%08lx, kernel_entry:0x%08lx\n", kernelStartAddr, kernelEndAddr, kernelEntryAddr);

    phyAddr_t kernelRoundUpEndAddr = pa_roundup(kernelEndAddr);
    /* Page list begin addr. */
    PageListBegin = (pframe_t *)kernelRoundUpEndAddr;

    ppn_t ppnBegin = convert_pa2ppn_cil(kernelStartAddr);
    ppn_t ppnEnd = convert_pa2ppn_cil(MEMORY_END);
    uint16_t ppnTotalAmount = ppnEnd - ppnBegin; /* Total amount of the ppn.*/

    /* This section of memory between end of roundup kernel and the begin of free memory is used for store page list.*/
    uint64_t memoryOfPageListUsed = sizeof(pframe_t) * ppnTotalAmount;

    /* True end of kernel used memory. */
    phyAddr_t freeMemoryBeginAddr = (phyAddr_t)PageListBegin + (phyAddr_t)memoryOfPageListUsed;

    uint64_t freeMemorySize = (uint64_t)(MEMORY_END - freeMemoryBeginAddr);
    ppn_t ppnFreeBegin = convert_pa2ppn_cil(freeMemoryBeginAddr);
    ppn_t ppnFreeEnd = ppnEnd;

    printf("physical memory map:\n");
    printf("memory: 0x%08lx, [0x%08lx, 0x%08x].\n", freeMemorySize, freeMemoryBeginAddr, MEMORY_END);
    printf("ppn: ppnTotalAmount:%08d, [ppnFreeBegin:%08ld, ppnFreeEnd:%08ld].\n", ppnTotalAmount, ppnFreeBegin, ppnFreeEnd);

    /* Set status of kernel memory and free memory for pages. */
    uint16_t cursor;
    for (cursor = 0; cursor < ppnFreeBegin - ppnBegin; cursor++)
    {
        PageListBegin[cursor].canBeAlloc = 0;
    }

    /* Initial the memory map and free lists. */
    pframe_t *freeFrameBegin = &(PageListBegin[cursor]);
    for (; cursor <= ppnTotalAmount; cursor++)
    {
        PageListBegin[cursor].canBeAlloc = 1;
        PageListBegin[cursor].reference = 0;
    }

    uint16_t ppnFreeAmount = ppnFreeEnd - ppnFreeBegin + 1;
    freeFrameBegin->nsize = ppnFreeAmount;

    INIT_LIST_HEAD((&FreeList.list_linker));
    FreeList.fnsize = ppnFreeAmount;
    list_add(&(freeFrameBegin->list_linker_inFreeList), &(FreeList.list_linker));

    INIT_LIST_HEAD((&FreeAList.list_linker));
    FreeAList.fnsize = ppnFreeAmount;
    list_add(&(freeFrameBegin->list_linker_inFreeAList), &(FreeAList.list_linker));

#if DEBUG_MMU_mm_init
    printf("ppnFreeAmount:%d\n", ppnFreeAmount);
    printf("FreeList.fnsize:%d, FreeAList.fnsize:%d\n", FreeList.fnsize, FreeAList.fnsize);
#endif

    kernelPa2Va_IdentityMapping(freeMemoryBeginAddr);
    enable_mmu();
    printf("mmu inited!\n");
}

pframe_t *mm_alloc(uint16_t nsize)
{
    pframe_t *ret = (pframe_t *)0;
    if (nsize > FreeList.fnsize)
    {
        printf("Frame has not been allocated!\n");
        return ret;
    }
    ret = BFallocator.bffa_deleteAndReinsert(nsize);
    if (ret != (pframe_t *)0)
    {
        pframe_t *currentFrame;
        for (currentFrame = ret; currentFrame != ret + nsize; currentFrame++)
        {
            currentFrame->canBeAlloc = 0;
        }
        phyAddr_t *dst;
        dst = (phyAddr_t *)convert_pframe2pa(ret);
        while (dst != (phyAddr_t *)convert_pframe2pa(ret + nsize))
        {
            *dst++ = 0;
        }
    }
    else
    {
        printf("Frame has not been allocated!\n");
    }

#if DEBUG_MMU_mm_alloc
    printf("mm_alloc::Frame has been allocated!ppn:%ld,pa:%08lx\n", convert_pframe2ppn(ret), convert_pframe2pa(ret));
#endif

    return ret;
}

void mm_dealloc(pframe_t *baseppn, uint16_t nsize)
{
    pframe_t *currentFrame;
    for (currentFrame = baseppn; currentFrame != baseppn + nsize; currentFrame++)
    {
        currentFrame->canBeAlloc = 1;
        currentFrame->reference = 0;
    }
    baseppn->nsize = nsize;

#if DEBUG_MMU_mm_dealloc
    ppn_t ppnDealloc;
    ppnDealloc = convert_pframe2ppn(baseppn);
#endif

    BFallocator.bffa_insertAndMerge(baseppn, nsize);

#if DEBUG_MMU_mm_dealloc
    printf("mm_dealloc::Frame has been deallocated!ppn:%ld,pa:%08lx,nsize:%d\n", ppnDealloc, convert_ppn2pa(ppnDealloc), nsize);
#endif
}

pframe_t *mm_allocOneFrame(void)
{
    return mm_alloc((u_int16_t)1);
}

void mm_deallocOneFrame(pframe_t *baseppn)
{
    mm_dealloc(baseppn, (uint16_t)1);
}

pte_t *mm_getPte(pframe_t *pageTable, virAddr_t va)
{
    return searchAndGetPteIfExists(pageTable, va, 3);
}

void mm_removePte(virAddr_t va, pte_t *pte)
{
    removePteFromPageTable(va, pte);
}

pte_t *mm_insertPte(pframe_t *pageTable, virAddr_t va, pteflg_t pteFlag)
{
    return insertPteIntoPageTable(pageTable, va, 3, pteFlag);
}

static pframe_t *deleteAndReinsert(uint16_t nsize)
{
    pframe_t *ret = (pframe_t *)0, *currentFrame;
    struct list_head *currentEntry; /* "Current*" is used for temp. */
    list_for_each(currentEntry, &(FreeList.list_linker))
    {
        currentFrame = list_entry(currentEntry, pframe_t, list_linker_inFreeList);
#if DEBUG_MMU_deleteAndReinsert
        printf("deleteAndReinsert::currentFrame ppn:%ld,nsize:%d\n", convert_pframe2ppn(currentFrame), currentFrame->nsize);
#endif
        if (currentFrame->nsize >= nsize)
        {
            ret = currentFrame;
            break;
        }
    }
    /* If found the free block we need. */
    if (ret != (pframe_t *)0)
    {
        /* Delete this entry in FreeList&FreeAList. */
        list_del(&(ret->list_linker_inFreeList));
        list_del(&(ret->list_linker_inFreeAList));
        /* There are remaining memories in this block. */
        if (ret->nsize > nsize)
        {
            pframe_t *reinsertFrame = ret + nsize;
            reinsertFrame->nsize = ret->nsize - nsize;
            if ((&(FreeList.list_linker))->next == &(FreeList.list_linker))
            {
                list_add(&(reinsertFrame->list_linker_inFreeList), &(FreeList.list_linker));
                list_add(&(reinsertFrame->list_linker_inFreeAList), &(FreeAList.list_linker));
                goto f1;
            }
            /* Reinsert the remaining block into the free lists. */
            list_for_each(currentEntry, &(FreeList.list_linker))
            {
                if ((list_entry(currentEntry, pframe_t, list_linker_inFreeList))->nsize >= reinsertFrame->nsize)
                {
                    /* Modify the remaining frame's entry in FreeList. */
                    list_add_tail(&(reinsertFrame->list_linker_inFreeList), currentEntry);
                    /* Modify this frame's entry in FreeAList. */
                    list_add(&(reinsertFrame->list_linker_inFreeAList), (ret->list_linker_inFreeAList).prev);
                    break;
                }
                /* "reinsertFrame" is the largest block in FreeList,add it into the FreeList at last. */
                else if ((list_entry(currentEntry, pframe_t, list_linker_inFreeList))->nsize < reinsertFrame->nsize && currentEntry->next == &(FreeList.list_linker))
                {
                    /* Modify the remaining frame's entry in FreeList. */
                    list_add(&(reinsertFrame->list_linker_inFreeList), currentEntry);
                    /* Modify this frame's entry in FreeAList. */
                    list_add(&(reinsertFrame->list_linker_inFreeAList), (ret->list_linker_inFreeAList).prev);
                    break;
                }
            }
        }
    f1:
        FreeList.fnsize = FreeList.fnsize - nsize;
        FreeAList.fnsize = FreeAList.fnsize - nsize;
    }
#if DEBUG_MMU_deleteAndReinsert
    printf("deleteAndReinsert::FreeList.fnsize:%d,FreeAList.fnsize:%d\n", FreeList.fnsize, FreeAList.fnsize);
#endif
    return ret;
}

static void insertAndMerge(pframe_t *baseppn, uint16_t nsize)
{
    pframe_t *currentFrame;
    struct list_head *currentEntry;

    /* Insert into the FreeAList frist,then merge if needed,finally insert into FreeList. */
    FreeAList.fnsize = FreeAList.fnsize + nsize;
    if (list_empty(&(FreeAList.list_linker)))
    {
        list_add(&(baseppn->list_linker_inFreeAList), &(FreeAList.list_linker));
    }
    else
    {
        list_for_each(currentEntry, &(FreeAList.list_linker))
        {
            currentFrame = list_entry(currentEntry, pframe_t, list_linker_inFreeAList);
            if (currentFrame > baseppn)
            {
                list_add_tail(&(baseppn->list_linker_inFreeAList), currentEntry);
                break;
            }
            /* "baseppn" is the highest addr in memory,add it into the FreeAList at last. */
            else if (currentEntry->next == &(FreeAList.list_linker))
            {
                list_add(&(baseppn->list_linker_inFreeAList), currentEntry);
                break;
            }
        }
    }

    /* After inserted into FreeAList,check if needs merge. */
    pframe_t *prevFrameInFreeAList, *nextFrameInFreeAList;
    if (FreeAList.list_linker.next == &(baseppn->list_linker_inFreeAList))
    {
        prevFrameInFreeAList = (pframe_t *)0;
    }
    else
    {
        prevFrameInFreeAList = list_entry((baseppn->list_linker_inFreeAList).prev, pframe_t, list_linker_inFreeAList);
    }
    if (FreeAList.list_linker.prev == &(baseppn->list_linker_inFreeAList))
    {
        nextFrameInFreeAList = (pframe_t *)0;
    }
    else
    {
        nextFrameInFreeAList = list_entry((baseppn->list_linker_inFreeAList).next, pframe_t, list_linker_inFreeAList);
    }
    pframe_t *mergedFrame = baseppn;

#if DEBUG_MMU_insertAndMerge
    printf("insertAndMerge::baseppn:%ld prevFrameInFreeAList:%ld nextFrameInFreeAList:%ld\n", convert_pframe2ppn(baseppn),
           convert_pframe2ppn(prevFrameInFreeAList), convert_pframe2ppn(nextFrameInFreeAList));
#endif

    pframe_t *frameClosestAfter = baseppn + baseppn->nsize;
    if (nextFrameInFreeAList != (pframe_t *)0)
    {
        if (frameClosestAfter == nextFrameInFreeAList) /* It means need merge with the after block. */
        {
            list_del(&(nextFrameInFreeAList->list_linker_inFreeAList));
            list_del(&(nextFrameInFreeAList->list_linker_inFreeList));
            baseppn->nsize = baseppn->nsize + nextFrameInFreeAList->nsize;
            mergedFrame = baseppn;
        }
    }
    if (prevFrameInFreeAList != (pframe_t *)0)
    {
        pframe_t *frameClosestForward = prevFrameInFreeAList + prevFrameInFreeAList->nsize;
        if (frameClosestForward == baseppn) /* It means need merge with the forwrd block. */
        {
            list_del(&(baseppn->list_linker_inFreeAList));
            list_del(&(prevFrameInFreeAList->list_linker_inFreeList));
            prevFrameInFreeAList->nsize = prevFrameInFreeAList->nsize + nsize;
            mergedFrame = prevFrameInFreeAList;
        }
    }

#if DEBUG_MMU_insertAndMerge
    printf("insertAndMerge::mergedFrame ppn:%ld,mergedFrame->nsize %d\n", convert_pframe2ppn(mergedFrame), mergedFrame->nsize);
#endif

    /* After merge,insert into FreeList. */
    FreeList.fnsize = FreeList.fnsize + nsize;
    if (list_empty(&(FreeList.list_linker)))
    {
        list_add(&(mergedFrame->list_linker_inFreeList), &(FreeList.list_linker));
    }
    else
    {
        list_for_each(currentEntry, &(FreeList.list_linker))
        {
            if ((list_entry(currentEntry, pframe_t, list_linker_inFreeList))->nsize >= mergedFrame->nsize)
            {
                list_add_tail(&(mergedFrame->list_linker_inFreeList), currentEntry);
                break;
            }
            /* "reinsertFrame" is the largest block in FreeList,add it into the FreeList at last. */
            else if ((list_entry(currentEntry, pframe_t, list_linker_inFreeList))->nsize < mergedFrame->nsize && currentEntry->next == &(FreeList.list_linker))
            {
                list_add(&(mergedFrame->list_linker_inFreeList), currentEntry);
                break;
            }
        }
    }
}

static void kernelPa2Va_IdentityMapping(phyAddr_t tKernelEndAddr)
{
    KernelLevel3PageTableFrame = mm_allocOneFrame(); /* Used for level3 page table. */
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
    ppn_t ppnKernelEnd;

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
    ppnKernelEnd = convert_pa2ppn_flr(tKernelEndAddr);

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
        cursorPte = mm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_text);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        mm_deallocOneFrame(cursorFrame);
    }

    for (; ppnCursor <= ppn_erodata; ppnCursor++)
    {
        cursorPte = mm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_rodata);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        mm_deallocOneFrame(cursorFrame);
    }

    for (; ppnCursor <= ppn_edata; ppnCursor++)
    {
        cursorPte = mm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_data);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        mm_deallocOneFrame(cursorFrame);
    }

    for (; ppnCursor <= ppn_ebss; ppnCursor++)
    {
        cursorPte = mm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_bss);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        mm_deallocOneFrame(cursorFrame);
    }

    for (; ppnCursor <= ppnKernelEnd; ppnCursor++)
    {
        cursorPte = mm_insertPte(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_data);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        mm_deallocOneFrame(cursorFrame);
    }
}

static pte_t *searchAndGetPteIfExists(pframe_t *pageTable, virAddr_t va, uint16_t level)
{
    pte_t *pageTableAddrPointer; /* Because essentially it is a pte array. [0]~[511] */
    vpn_t currentVpn;
    pte_t *currentPte;
    pageTableAddrPointer = (pte_t *)convert_pframe2pa(pageTable);
    currentVpn = convert_va2vpn(va, level);
    currentPte = &(pageTableAddrPointer[currentVpn]);
    if (!pte_is_valid(*currentPte))
    {
        return (pte_t *)0;
    }
    else
    {
        if (level > 1)
        {
            ppn_t ppnOfNextLevelPageTable;
            ppnOfNextLevelPageTable = pte_get_ppn(*currentPte);
            return searchAndGetPteIfExists(convert_ppn2pframe(ppnOfNextLevelPageTable), va, level - 1);
        }
        else /* level == 1 */
        {
            return currentPte;
        }
    }
}

static void removePteFromPageTable(virAddr_t va, pte_t *pte)
{
    if (pte_is_valid(*pte))
    {
        pframe_t *currentFrame;
        currentFrame = convert_pte2pframe(*pte);
        currentFrame->reference = currentFrame->reference - 1;
        if (currentFrame->reference == 0)
        {
            mm_deallocOneFrame(currentFrame);
        }
        *pte = (pte_t)0;
        refreshTLB(va);
    }
}

/* This function will auto create middle pte if needed. */
static pte_t *insertPteIntoPageTable(pframe_t *pageTable, virAddr_t va, uint16_t level, pteflg_t pteFlag)
{
    pte_t *pageTableAddrPointer;
    vpn_t currentVpn;
    pte_t *currentPte;
    pageTableAddrPointer = (pte_t *)convert_pframe2pa(pageTable);
    currentVpn = convert_va2vpn(va, level);
    currentPte = &(pageTableAddrPointer[currentVpn]);
    if (!pte_is_valid(*currentPte)) /* Current pte does not exist. */
    {
        pframe_t *newFrameOfNextLevelPageTable;
        pte_t newPteOfThisLevelPageTable;
        newFrameOfNextLevelPageTable = mm_allocOneFrame();
        newFrameOfNextLevelPageTable->reference = newFrameOfNextLevelPageTable->reference + 1;
        if (newFrameOfNextLevelPageTable == (pframe_t *)0)
        {
            printf("Failed to insert pte:No more free memories!\n");
            return (pte_t *)0;
        }
        /* When level is 1,it means this is level1 page table so newFrameOfNextLevelPageTable is the physical frame but not the page table. */
        if (level == 1)
        {
            newPteOfThisLevelPageTable = pte_create(convert_pframe2ppn(newFrameOfNextLevelPageTable), pteFlag);
            *currentPte = newPteOfThisLevelPageTable;
            return currentPte;
        }
        /* Middle pte and page table. */
        else
        {
            newPteOfThisLevelPageTable = pte_create(convert_pframe2ppn(newFrameOfNextLevelPageTable), PTE_V);
            *currentPte = newPteOfThisLevelPageTable;
        }
    }
    /* Else the current pte (middle page table) exists. */
    else
    {
        /* Target pte(currentPte) is already exists,modify it. */
        if (level == 1)
        {
            pte_t tempPte;
            tempPte = *currentPte;
            *currentPte = pte_create(convert_pframe2ppn(convert_pte2pframe(tempPte)), pteFlag);
            return currentPte;
        }
    }
    /* The middle page table already exists. */
    return insertPteIntoPageTable(convert_pte2pframe(*currentPte), va, level - 1, pteFlag);
}

static inline void pteChangeppn(pte_t *pte, ppn_t ppn)
{
    *pte = pte_create(ppn, pte_get_flag(*pte));
}

static void enable_mmu(void)
{
    write_csr(satp, SATPMODE_RV39 | convert_pframe2ppn(KernelLevel3PageTableFrame));
}
