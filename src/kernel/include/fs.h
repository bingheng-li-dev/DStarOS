#ifndef _FS_H_
#define _FS_H_

#include <stdint.h>

#include "pmm.h"

void fs_init(void);
int16_t swapfs_read(pte_t pte, pframe_t *frame);
int16_t swapfs_write(pte_t pte, pframe_t *frame);

#endif