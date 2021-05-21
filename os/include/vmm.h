#ifndef _VMM_H
#define _VMM_H

#include "pmm.h"
#include "rbtree.h"
#include "rbtree_augmented.h"

#define VMP_R 0x1
#define VMP_W 0x2
#define VMP_X 0x4

typedef uint16_t pgprot_t;

typedef struct continuousVmAreaStruct vma_t;
typedef struct processVmmStruct mm_t;

struct processVmmStruct
{
    uint16_t mapCount;   /* How many process shared this vmm. */
    pframe_t *pageTable; /* Process's base page table. */
    vma_t *lastAccess;
    struct list_head mmap;         /* List head of its continuousVmAreaStruct. */
    struct rb_root mm_rbtree_root;
    // struct list_head clockList;  /* Used for page replacement algorithm(enhanced clock algorithm). */
};

struct continuousVmAreaStruct
{
    mm_t *whichPro;      /* This vma is belong to which process.*/
    virAddr_t vaStart;   /* Start virtual address.WITHIN the vma. */
    virAddr_t vaEnd;     /* End virtual address.OUTSIDE the vma. */
    pgprot_t vmPageProt; /* Protect flag. */
    struct list_head list_linker;
    struct rb_node rbtree_node;
};

extern pframe_t *KernelLevel3PageTableFrame;
extern bool readyToSwap;


extern int16_t swapfs_read(pte_t pte, pframe_t *frame);
extern int16_t swapfs_write(pte_t pte, pframe_t *frame);

void vmm_init(void);
mm_t *vmm_mmCreate(void);
void vmm_mmDestroy(mm_t *mm);
vma_t *vmm_vmaCreate(virAddr_t va_start, virAddr_t va_end, pgprot_t flag);
void vmm_vmaDestroy(vma_t *vma);
vma_t *vmm_vmaGet(mm_t *mm, virAddr_t va);
void vmm_vmaInsert(mm_t *mm, vma_t *vma);
void vmm_pageFaultHander(virAddr_t badva);
/* Used for enhanced clock algorithm in page replacement algorithms. */
uint16_t vmm_swapOut(mm_t* mm,pframe_t** framePtr,uint16_t n);

#endif