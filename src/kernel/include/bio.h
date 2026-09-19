/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _BIO_H_
#define _BIO_H_

#include <stdint.h>

#include "bdev.h"

typedef struct bio_stats
{
    uint64_t hits;              /* 命中缓存的块数 */
    uint64_t misses;            /* 未命中、需要读设备的块数 */
    uint64_t dev_reads;         /* 下发给设备的读请求次数（连续未命中合并为一次） */
    uint64_t dev_writes;        /* 下发给设备的写请求次数 */
    uint64_t dev_write_blocks;  /* 写入设备的块数 */
    uint64_t evictions;         /* 淘汰掉的有效缓存块数 */
} bio_stats_t;

void bio_init(void);
int  bio_read(bdev_t *dev, uint64_t lba, uint8_t *buf, uint32_t count);
int  bio_write(bdev_t *dev, uint64_t lba, const uint8_t *buf, uint32_t count);
void bio_get_stats(bio_stats_t *out);

#endif
