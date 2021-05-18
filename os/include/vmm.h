#ifndef _VMM_H
#define _VMM_H

#include "pmm.h"

#define VMP_R 0x1
#define VMP_W 0x2
#define VMP_X 0x4

typedef uint16_t pgprot_t;

typedef struct continuousVmmAreaStruct cvmmas_t;
typedef struct processVmmStruct pvmms_t;

struct processVmmStruct
{
    uint16_t mapCount;     /* How many process shared this vmm. */
    pframe_t *pageTable;   /* Process's base page table. */
    cvmmas_t *vmmAreaList; /* List head of its continuousVmmAreaStruct. */
};

struct continuousVmmAreaStruct
{
    pvmms_t *whichPro;   /* This vma is belong to which process.*/
    virAddr_t vmStart;   /* Start virtual address. */
    virAddr_t vmEnd;     /* End virtual address */
    pgprot_t vmPageProt; /* Protect flag. */
    struct list_head list_linker;
};

extern uint64_t swapfs_read(pte_t pte, pframe_t *frame);
extern uint64_t swapfs_write(pte_t pte, pframe_t *frame);

void vmm_init(void);
void vmm_pageFaultHander(virAddr_t va);
void vmm_pageReplacement_EnhancedClock();

#endif