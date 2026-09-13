#ifndef _SDMMC_H_
#define _SDMMC_H_

#include <stdint.h>

#include "debug.h"

int sdmmc_init(void);
int sdmmc_read_blocks(uint64_t lba, uint8_t *buf, uint32_t count);
int sdmmc_write_blocks(uint64_t lba, const uint8_t *buf, uint32_t count);

#if DEBUG_SDMMC_PROBE
void sdmmc_probe(void);
#endif

#if DEBUG_SDMMC_WRITE_TEST
void sdmmc_write_test(void);
#endif

#endif
