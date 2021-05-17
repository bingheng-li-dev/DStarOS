#ifndef _VMM_H
#define _VMM_H

#include "pmm.h"

extern uint64_t swapfs_read(pte_t pte,pframe_t *frame);
extern uint64_t swapfs_write(pte_t pte,pframe_t *frame);

#endif