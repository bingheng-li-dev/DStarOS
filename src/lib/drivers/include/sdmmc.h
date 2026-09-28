/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _SDMMC_H_
#define _SDMMC_H_

#include <stdint.h>
#include <stdbool.h>

#include "debug.h"

typedef struct sdmmc_stats
{
    uint64_t cmd17;
    uint64_t cmd18;
    uint64_t cmd24;
    uint64_t cmd25;
    uint64_t cmd12_manual;   /* 多块传输后等不到自动 CMD12、手动补发的次数 */
    uint64_t blocks_read;
    uint64_t blocks_written;
} sdmmc_stats_t;

int sdmmc_init(void);
int sdmmc_read_blocks(uint64_t lba, uint8_t *buf, uint32_t count);
int sdmmc_write_blocks(uint64_t lba, const uint8_t *buf, uint32_t count);
void sdmmc_get_stats(sdmmc_stats_t *out);

#if DEBUG_SDMMC_PROBE
void sdmmc_probe(void);
void sdmmc_set_multiblock(bool enable);
#endif

#if DEBUG_SDMMC_WRITE_TEST
void sdmmc_write_test(void);
#endif

#endif /* _SDMMC_H_ */
