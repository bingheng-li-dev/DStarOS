#include "vmm.h"
#include "pmm.h"
#include "kmalloc.h"
#include "console.h"

/* Kernel base page table. */
pframe_t *KernelLevel3PageTableFrame;
bool readyToSwap = false;
/* @warning nowhere Assigned for now! */
mm_t *currentProcessMm;

/* Must be called first before using functions about page table. */
static void kernelPa2Va_IdentityMapping(void);
static void enable_mmu(void) __attribute__((used));
static inline void pteChangeppn(pte_t *pte, ppn_t ppn);
static vma_t *rbtree_vmaSearch(struct rb_root *root, virAddr_t va);
static bool rbtree_vmaInsert(struct rb_root *root, vma_t *vma);
/* Return true if the two vma overlap. */
static bool isVmaOverlap(vma_t *vmaprev, vma_t *vma);
/* Search next victim. */
static uint16_t swap_out_victim(mm_t *mm, pframe_t **currentFramePtr);
static void swap_init(void);
static void swap_in(mm_t *mm, virAddr_t va, pframe_t **resultFramePtr);

void vmm_init(void)
{
    kernelPa2Va_IdentityMapping();
    swap_init();
    enable_mmu();
    printf("vmm inited!\n");
}

mm_t *vmm_mmCreate(void)
{
    mm_t *ret;
    ret = NULL;
    ret = kmalloc(sizeof(mm_t));
    if (ret != NULL)
    {
        ret->lastAccess = NULL;
        ret->mapCount = 0;
        ret->pageTable = (pframe_t *)0;
        INIT_LIST_HEAD(&(ret->mmap));
        // INIT_LIST_HEAD(&(ret->clockList));
        ret->mm_rbtree_root = RB_ROOT;
    }
    return ret;
}

void vmm_mmDestroy(mm_t *mm)
{
    struct list_head *temp, *current;
    vma_t *currentVma;
    list_for_each_safe(current, temp, &(mm->mmap))
    {
        currentVma = list_entry(current, vma_t, list_linker);
        rb_erase(&(currentVma->rbtree_node), &(mm->mm_rbtree_root));
        list_del(current);
        vmm_vmaDestroy(currentVma);
    }
    kfree(mm);
    mm = NULL;
}

vma_t *vmm_vmaCreate(virAddr_t va_start, virAddr_t va_end, pgprot_t flag)
{
    vma_t *ret;
    ret = NULL;
    ret = kmalloc(sizeof(vma_t));
    if (ret != NULL)
    {
        ret->vaStart = va_start;
        ret->vaEnd = va_end;
        ret->vmPageProt = flag;
        ret->whichPro = NULL;
    }
    return ret;
}

void vmm_vmaDestroy(vma_t *vma)
{
    if (vma != NULL)
    {
        kfree(vma);
        vma = NULL;
    }
}

vma_t *vmm_vmaGet(mm_t *mm, virAddr_t va)
{
    vma_t *ret = NULL;
    if (mm != NULL)
    {
        ret = mm->lastAccess;
        if (!(ret != NULL && ret->vaStart <= va && ret->vaEnd > va))
        {
            ret = rbtree_vmaSearch(&(mm->mm_rbtree_root), va);
        }
        if (ret != NULL)
        {
            mm->lastAccess = ret;
        }
    }
    return ret;
}

void vmm_vmaInsert(mm_t *mm, vma_t *vma)
{
    if (vma->vaEnd < vma->vaStart)
    {
        return;
    }
    vma_t *current = NULL, *prevVmaInList = NULL, *nextVmaInList = NULL;
    struct list_head *currentEntry;
    /* One situation that list is empty has been included here as well. */
    list_for_each(currentEntry, &(mm->mmap))
    {
        current = list_entry(currentEntry, vma_t, list_linker);
        prevVmaInList = list_entry(currentEntry->prev, vma_t, list_linker);
        if (prevVmaInList->vaStart > current->vaStart)
        {
            current = prevVmaInList;
            break;
        }
    }
    if ((current->list_linker).next != &(mm->mmap))
    {
        nextVmaInList = list_entry((current->list_linker).next, vma_t, list_linker);
        if (isVmaOverlap(prevVmaInList, current))
        {
            printf("Invalid va!Overlaped with the prev.\n");
            while (1)
                ;
        }
    }
    if ((current->list_linker).prev != &(mm->mmap))
    {
        prevVmaInList = list_entry((current->list_linker).prev, vma_t, list_linker);
        if (isVmaOverlap(current, nextVmaInList))
        {
            printf("Invalid va!Overlaped with the next.\n");
            while (1)
                ;
        }
    }
    vma->whichPro = mm;
    list_add(&(vma->list_linker), &(mm->mmap));
    if (!rbtree_vmaInsert(&(mm->mm_rbtree_root), vma))
    {
        printf("Failed to insert into rbtree of vma!\n");
        while (1)
            ;
    }
    mm->mapCount = mm->mapCount + 1;
}

void vmm_pageFaultHander(virAddr_t badva)
{
    extern mm_t *currentProcessMm;
    swap_in(currentProcessMm, badva, NULL);
}

uint16_t vmm_swapOut(mm_t *mm, pframe_t **framePtr, uint16_t n)
{
    // int i;
    // for (i = 0; i != n; ++i)
    // {
    //     virAddr_t va;
    //     pframe_t *page = NULL;
    //     int r = swap_out_victim(mm, &page);
    //     if (r != 0)
    //     {
    //         printf("i %d, swap_out: call swap_out_victim failed\n", i);
    //         break;
    //     }
    //     va = page->va;
    //     pte_t *ptep = pmm_pteGet(mm->pageTable, va);
    //     if (swapfs_write((page->va / PGSIZE + 1) << 8, page) != 0)
    //     {
    //         printf("SWAP: failed to save\n");
    //         continue;
    //     }
    //     else
    //     {
    //         printf("swap_out: i %d, store page in vaddr %08lx to disk swap entry %ld\n", i, va, page->va / PGSIZE + 1);
    //         *ptep = (page->va / PGSIZE + 1) << 8;
    //         kfree((phyAddr_t *)convert_pframe2pa(page));
    //     }
    //     refreshTLB(va);
    // }
    // return i;
    swap_out_victim(NULL,NULL);
    return 0;
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
        cursorPte = pmm_pteInsert(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_text);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        kfree((phyAddr_t *)convert_pframe2pa(cursorFrame));
    }

    for (; ppnCursor <= ppn_erodata; ppnCursor++)
    {
        cursorPte = pmm_pteInsert(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_rodata);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        kfree((phyAddr_t *)convert_pframe2pa(cursorFrame));
    }

    for (; ppnCursor <= ppn_edata; ppnCursor++)
    {
        cursorPte = pmm_pteInsert(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_data);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        kfree((phyAddr_t *)convert_pframe2pa(cursorFrame));
    }

    for (; ppnCursor <= ppn_ebss; ppnCursor++)
    {
        cursorPte = pmm_pteInsert(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_bss);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        kfree((phyAddr_t *)convert_pframe2pa(cursorFrame));
    }

    /* All remaining physical memories should be identically mapped to virtual address in order kernel directly accesses physical memories. */
    for (; ppnCursor <= ppnEnd; ppnCursor++)
    {
        cursorPte = pmm_pteInsert(kernelBasePageTable, convert_ppn2pa(ppnCursor), kernelPageFlag_data);
        cursorFrame = convert_pte2pframe(*cursorPte);
        pteChangeppn(cursorPte, ppnCursor);
#if DEBUG_MMU_kernelPa2Va_IdentityMapping
        printf("kernelPa2Va_IdentityMapping::Changed cursorPte pa:%08lx,pte:%lx\n\n", (phyAddr_t)cursorPte, *cursorPte);
#endif
        kfree((phyAddr_t *)convert_pframe2pa(cursorFrame));
    }
}

static void enable_mmu(void)
{
    write_csr(satp, SATPMODE_RV39 | convert_pframe2ppn(KernelLevel3PageTableFrame));
}

static inline void pteChangeppn(pte_t *pte, ppn_t ppn)
{
    *pte = pteCreate(ppn, pteGetFlag(*pte));
}

static vma_t *rbtree_vmaSearch(struct rb_root *root, virAddr_t va)
{
    struct rb_node *node;
    vma_t *ret = NULL;
    node = root->rb_node;
    while (node)
    {
        vma_t *temp = container_of(node, vma_t, rbtree_node);
        if (temp->vaEnd > va)
        {
            ret = temp;
            if (temp->vaStart <= va)
                break;
            node = node->rb_left;
        }
        else
            node = node->rb_right;
    }
    return ret;
}

static bool rbtree_vmaInsert(struct rb_root *root, vma_t *vma)
{
    struct rb_node **new = &(root->rb_node), *parent = NULL;

    /* Figure out where to put new node */
    while (*new)
    {
        vma_t *this = container_of(*new, vma_t, rbtree_node);
        parent = *new;
        if (vma->vaStart < this->vaStart)
            new = &((*new)->rb_left);
        else if (vma->vaStart > this->vaStart)
            new = &((*new)->rb_right);
        else
            return false;
    }

    /* Add new node and rebalance tree. */
    rb_link_node(&vma->rbtree_node, parent, new);
    rb_insert_color(&vma->rbtree_node, root);

    return true;
}

static bool isVmaOverlap(vma_t *vmaprev, vma_t *vma)
{
    if (vmaprev->vaEnd <= vmaprev->vaStart)
    {
        return true;
    }
    if (vma->vaEnd <= vma->vaStart)
    {
        return true;
    }
    if (vmaprev->vaEnd > vma->vaStart)
    {
        return true;
    }
    return false;
}

static void swap_init(void)
{
    readyToSwap = true;
}

static uint16_t swap_out_victim(mm_t *mm, pframe_t **currentFramePtr)
{
    //     struct list_head* ptr;
    //     list_for_each(ptr,&(mm->clockPtr->list_linker))
    //     {

    //     }
    return 0;
}

static void swap_in(mm_t *mm, virAddr_t va, pframe_t **resultFramePtr)
{
    // pframe_t *ret = pmm_allocOneFrame();
    // if (ret != (pframe_t *)0)
    // {
    //     int16_t rt;
    //     rt = swapfs_read(*pmm_pteGet(mm->pageTable, va), ret);
    //     if (rt == 0)
    //     {
    //         *resultFramePtr = ret;
    //         return;
    //     }
    // }
    // printf("Swap in failed!\n");
    // while (1)
    //     ;
}
