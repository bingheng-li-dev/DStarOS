#include "pmm.h"
#include "vmm.h"
#include "stringops.h"
#include "console.h"
#include "cpu.h"

//@TODO:recyclePageTableRecursively(,cnt,...);

pframe_t *PageListBegin;
fslist_t FreeList;  /* Free memories will arrange from small size to large size,used for best fit. */
fslist_t FreeAList; /* Free memories will arrange from small addr to large addr,used for merge. */
/* The following vars are used for micro memory alloc. */
pframe_t *MicroPhysicalMemoryPoolBase;
phyAddr_t *PtrTableAddr;          /* Used for restore ptr who used micro mem. */
bool MicroMemUsage[64] = {false}; /* 64位对应64块小段内存，记录小块内存的使用情况。 */
osslock_t PmmLock;

static pframe_t *deleteAndReinsert(uint16_t nsize);
static void insertAndMerge(pframe_t *baseppn, uint16_t nsize);
/* Return null if not exists.When using:@param pageTable should be base page table,@param level should be 3. */
static pte_t *searchAndGetPteIfExists(pframe_t *pageTable, virAddr_t va, uint16_t level);
/* Delete the pte which refers to this virtual addr (param va) and dealloc the mapping pframe of this pte if needed. */
static void removePteFromPageTable(virAddr_t va, pte_t *pte);
/* Turn the given va into vpns and insert them into different level page tables. Return null if failed,else return the pte of page table level1. */
static pte_t *insertPteIntoPageTableRecursively(pframe_t *pageTable, virAddr_t va, uint16_t level, pteflg_t pteFlag);
/* The following funs are used for micro memory alloc. */
static void initMicroPhysicalMemoryPool(void);

const bffa_t BFallocator = {
    .bffa_deleteAndReinsert = deleteAndReinsert,
    .bffa_insertAndMerge = insertAndMerge,
};

void physicalMemoryManagementInit(void)
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

    MicroPhysicalMemoryPoolBase = NULL;
    initMicroPhysicalMemoryPool();
    spinlockInit(&PmmLock);
    printf("physicalMemoryManagement inited!\n");
}

void *alloc(uint16_t nsize)
{
    pframe_t *ret = NULL;
    if (nsize > FreeList.fnsize)
    {
        printf("Frame has not been allocated!\n");
        printf("Begin page replacement algorithm...\n");
        goto f1;
    }
    while (1)
    {
        // extern mm_t *currentProcessMm;
        ret = BFallocator.bffa_deleteAndReinsert(nsize);
        if (ret != NULL || readyToSwap == false || nsize > 1)
        {
            break;
        }
        // vmm_swapOut(currentProcessMm, &ret, nsize);
    }

    if (ret != NULL)
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

        /* "ret->nsize" restores the size of this alloced block which is convenient to free block. */
        ret->nsize = nsize;
    }
    else
    {
        //在实现页面置换算之前，这里只能panic
        panic("Frame has not been allocated!\n");
    }

#if DEBUG_MMU_mm_alloc
    printf("alloc::Frame has been allocated!ppn:%ld,pa:%08lx\n", convert_pframe2ppn(ret), convert_pframe2pa(ret));
#endif

f1:
    return ret;
}

void *allocOneFrame(void)
{
    return alloc((uint16_t)1);
}

pte_t *getPTE(pframe_t *pageTable, virAddr_t va)
{
    return searchAndGetPteIfExists(pageTable, va, 3);
}

void removePTE(virAddr_t va, pte_t *pte)
{
    removePteFromPageTable(va, pte);
}

pte_t *insertPTE(pframe_t *pageTable, virAddr_t va, pteflg_t pteFlag)
{
    return insertPteIntoPageTableRecursively(pageTable, va, 3, pteFlag);
}

void *microAlloc(uint64_t size)
{
    if (size > 1024)
    {
        goto f2;
    }
    uint16_t offset;
    if (size <= 32)
    {
        offset = 0;
    }
    else if (32 < size && size <= 64)
    {
        offset = 32;
    }
    else if (64 < size && size <= 128)
    {
        offset = 48;
    }
    else if (128 < size && size <= 256)
    {
        offset = 56;
    }
    else if (256 < size && size <= 512)
    {
        offset = 60;
    }
    else /* 512 < size && size <= 1024 */
    {
        offset = 62;
    }
    uint16_t position;
    for (position = offset; position <= 63; position++)
    {
        if (MicroMemUsage[position] == false)
        {
            phyAddr_t *dst;
            dst = (phyAddr_t *)PtrTableAddr[position];
            while (dst != (phyAddr_t *)PtrTableAddr[position + (uint16_t)1])
            {
                *dst++ = 0;
            }
            MicroMemUsage[position] = true;
#if DEBUG_MMU_microAlloc
            printf("microAlloc::offset:%d\tusage:%ld\tposition:%ld\n", offset, MicroMemUsage[position], position);
#endif
            goto f1;
        }
    }
    goto f2;
f1:
    return (void *)PtrTableAddr[position];
f2:
    return NULL;
}

bool microDemalloc(void *ptr)
{
    uint16_t position;
    for (position = 0; position <= 63; position++)
    {
        if (PtrTableAddr[position] == (phyAddr_t)ptr && MicroMemUsage[position] == true)
        {
            MicroMemUsage[position] = false;
            return true;
        }
    }
    return false;
}

void dealloc(pframe_t *baseppn)
{
    pframe_t *currentFrame;
    uint16_t nsize;
    nsize = baseppn->nsize;
    for (currentFrame = baseppn; currentFrame != baseppn + nsize; currentFrame++)
    {
        currentFrame->canBeAlloc = 1;
        currentFrame->reference = 0;
    }

#if DEBUG_MMU_mm_dealloc
    ppn_t ppnDealloc;
    ppnDealloc = convert_pframe2ppn(baseppn);
#endif

    BFallocator.bffa_insertAndMerge(baseppn, nsize);

#if DEBUG_MMU_mm_dealloc
    printf("dealloc::Frame has been deallocated!ppn:%ld,pa:%08lx,nsize:%d\n", ppnDealloc, convert_ppn2pa(ppnDealloc), nsize);
#endif
}

static pframe_t *deleteAndReinsert(uint16_t nsize)
{
    pframe_t *ret = NULL, *currentFrame;
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
    if (ret != NULL)
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

/* @param baseppn 被回收的物理块的首个物理页pframe_t地址
 * @param nsize 被回收的物理块的大小（含有几个物理页）
 */
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
        prevFrameInFreeAList = NULL;
    }
    else
    {
        prevFrameInFreeAList = list_entry((baseppn->list_linker_inFreeAList).prev, pframe_t, list_linker_inFreeAList);
    }
    if (FreeAList.list_linker.prev == &(baseppn->list_linker_inFreeAList))
    {
        nextFrameInFreeAList = NULL;
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

    /* 先与后面的合并，因为可能存在需要同时合并前面和后面的情况。 */
    pframe_t *frameClosestAfter = baseppn + baseppn->nsize;
    if (nextFrameInFreeAList != NULL)
    {
        if (frameClosestAfter == nextFrameInFreeAList) /* It means need merge with the after block. */
        {
            list_del(&(nextFrameInFreeAList->list_linker_inFreeAList));
            list_del(&(nextFrameInFreeAList->list_linker_inFreeList));
            /* 注意baseppn->nsize在此时发生了变化，变成了与后面合并后的总的nsize大小，要使用回收的大小使用参数nsize。 */
            baseppn->nsize = baseppn->nsize + nextFrameInFreeAList->nsize;
            mergedFrame = baseppn;
        }
    }
    if (prevFrameInFreeAList != NULL)
    {
        pframe_t *frameClosestForward = prevFrameInFreeAList + prevFrameInFreeAList->nsize;
        if (frameClosestForward == baseppn) /* It means need merge with the forwrd block. */
        {
            list_del(&(baseppn->list_linker_inFreeAList));
            list_del(&(prevFrameInFreeAList->list_linker_inFreeList));
            prevFrameInFreeAList->nsize = prevFrameInFreeAList->nsize + baseppn->nsize;
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

static pte_t *searchAndGetPteIfExists(pframe_t *pageTable, virAddr_t va, uint16_t level)
{
    pte_t *pageTableAddrPointer; /* Because essentially it is a pte array. [0]~[511] */
    vpn_t currentVpn;
    pte_t *currentPte;
    pageTableAddrPointer = (pte_t *)convert_pframe2pa(pageTable);
    currentVpn = convert_va2vpn(va, level);
    currentPte = &(pageTableAddrPointer[currentVpn]);
    if (!pteIsValid(*currentPte))
    {
        return (pte_t *)0;
    }
    else
    {
        if (level > 1)
        {
            ppn_t ppnOfNextLevelPageTable;
            ppnOfNextLevelPageTable = pteGetPpn(*currentPte);
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
    if (pteIsValid(*pte))
    {
        pframe_t *currentFrame;
        currentFrame = convert_pte2pframe(*pte);
        currentFrame->reference = currentFrame->reference - 1;
        if (currentFrame->reference == 0)
        {
            dealloc(currentFrame);
        }
        *pte = (pte_t)0;
        refreshTLB(va);
    }
}

/* This function will auto create middle pte if needed. */
static pte_t *insertPteIntoPageTableRecursively(pframe_t *pageTable, virAddr_t va, uint16_t level, pteflg_t pteFlag)
{
    pte_t *pageTableAddrPointer;
    vpn_t currentVpn;
    pte_t *currentPte;
    pageTableAddrPointer = (pte_t *)convert_pframe2pa(pageTable);
    currentVpn = convert_va2vpn(va, level);
    currentPte = &(pageTableAddrPointer[currentVpn]);
    if (!pteIsValid(*currentPte)) /* Current pte does not exist. */
    {
        pframe_t *newFrameOfNextLevelPageTable;
        pte_t newPteOfThisLevelPageTable;
        newFrameOfNextLevelPageTable = allocOneFrame();
        newFrameOfNextLevelPageTable->reference = newFrameOfNextLevelPageTable->reference + 1;
        if (newFrameOfNextLevelPageTable == NULL)
        {
            printf("Failed to insert pte:No more free memories!\n");
            return (pte_t *)0;
        }
        /* When level is 1,it means this is level1 page table so newFrameOfNextLevelPageTable is the physical frame but not the page table. */
        if (level == 1)
        {
            newPteOfThisLevelPageTable = pteCreate(convert_pframe2ppn(newFrameOfNextLevelPageTable), pteFlag);
            newFrameOfNextLevelPageTable->va = va;
            *currentPte = newPteOfThisLevelPageTable;
            return currentPte;
        }
        /* Middle pte and page table. */
        else
        {
            newPteOfThisLevelPageTable = pteCreate(convert_pframe2ppn(newFrameOfNextLevelPageTable), PTE_V);
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
            *currentPte = pteCreate(convert_pframe2ppn(convert_pte2pframe(tempPte)), pteFlag);
            convert_pte2pframe(tempPte)->va = va;
            return currentPte;
        }
    }
    /* The middle page table already exists. */
    return insertPteIntoPageTableRecursively(convert_pte2pframe(*currentPte), va, level - 1, pteFlag);
}

static void initMicroPhysicalMemoryPool(void)
{
    MicroPhysicalMemoryPoolBase = alloc(2); /* Total size 8192 byte. */
    if (MicroPhysicalMemoryPoolBase == NULL)
    {
        panic("MicroPhysicalMemoryPool init failed!\n");
    }
    else
    {
        phyAddr_t *poolBaseAddr;
        poolBaseAddr = (phyAddr_t *)convert_pframe2pa(MicroPhysicalMemoryPoolBase);
        *poolBaseAddr = 0x0;
        /* When makes a pointer +1,++,-- and etc , that pointer will moves sizeof(T) bytes. */
        /* So "PtrTableAddr" is offset to "poolBaseAddr" 8 bytes(64 bits). */
        PtrTableAddr = poolBaseAddr + 1;
        /* "PtrTableAddr" restores each micro memory's begin address,so "PtrTableAddr" is a array of phyAddr_t[64]. */
        /* "poolBeginAddr" is the begin address of these micro memories. */
        phyAddr_t *poolBeginAddr = poolBaseAddr + 128;
#if DEBUG_MMU_initMicroPhysicalMemoryPool
        printf("initMicroPhysicalMemoryPool::poolBaseAddr:%08lx poolBeginAddr:%08lx\n", (phyAddr_t)poolBaseAddr, (phyAddr_t)poolBeginAddr);
#endif
        uint16_t cursor;
        for (cursor = 0; cursor <= 31; cursor++)
        {
            PtrTableAddr[cursor] = (phyAddr_t)poolBeginAddr + cursor * 32;
#if DEBUG_MMU_initMicroPhysicalMemoryPool
            printf("initMicroPhysicalMemoryPool::PtrTableAddr[%d]:%08lx\n", cursor, PtrTableAddr[cursor]);
#endif
        }
        for (; cursor <= 47; cursor++)
        {
            PtrTableAddr[cursor] = PtrTableAddr[31] + cursor * 64;
#if DEBUG_MMU_initMicroPhysicalMemoryPool
            printf("initMicroPhysicalMemoryPool::PtrTableAddr[%d]:%08lx\n", cursor, PtrTableAddr[cursor]);
#endif
        }
        for (; cursor <= 55; cursor++)
        {
            PtrTableAddr[cursor] = PtrTableAddr[47] + cursor * 128;
#if DEBUG_MMU_initMicroPhysicalMemoryPool
            printf("initMicroPhysicalMemoryPool::PtrTableAddr[%d]:%08lx\n", cursor, PtrTableAddr[cursor]);
#endif
        }
        for (; cursor <= 59; cursor++)
        {
            PtrTableAddr[cursor] = PtrTableAddr[55] + cursor * 256;
#if DEBUG_MMU_initMicroPhysicalMemoryPool
            printf("initMicroPhysicalMemoryPool::PtrTableAddr[%d]:%08lx\n", cursor, PtrTableAddr[cursor]);
#endif
        }
        for (; cursor <= 61; cursor++)
        {
            PtrTableAddr[cursor] = PtrTableAddr[59] + cursor * 512;
#if DEBUG_MMU_initMicroPhysicalMemoryPool
            printf("initMicroPhysicalMemoryPool::PtrTableAddr[%d]:%08lx\n", cursor, PtrTableAddr[cursor]);
#endif
        }
        for (; cursor <= 63; cursor++)
        {
            PtrTableAddr[cursor] = PtrTableAddr[61] + cursor * 1024;
#if DEBUG_MMU_initMicroPhysicalMemoryPool
            printf("initMicroPhysicalMemoryPool::PtrTableAddr[%d]:%08lx\n", cursor, PtrTableAddr[cursor]);
#endif
        }
        printf("microPhysicalMemoryPool inited!\n");
    }
}
