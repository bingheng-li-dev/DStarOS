/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _BDEV_H_
#define _BDEV_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BDEV_BLOCK_SIZE 512
#define BDEV_MAX        4

struct bdev;

typedef struct bdev_ops
{
    int (*init)(struct bdev *dev);
    int (*read_blocks)(struct bdev *dev, uint64_t lba, uint8_t *buf, uint32_t count);
    int (*write_blocks)(struct bdev *dev, uint64_t lba, const uint8_t *buf, uint32_t count);
} bdev_ops_t;

typedef struct bdev
{
    const char       *name;
    const bdev_ops_t *ops;
    uint32_t          id;         /* 注册序号，即 FatFS 的物理驱动器号；缓存按 (id, lba) 作键 */
    uint64_t          nr_blocks;  /* init 成功后由驱动填写 */
    bool              ready;
    bool              read_only;
} bdev_t;

int     bdev_register(bdev_t *dev, uint32_t id);
bdev_t *bdev_get(uint32_t id);
int     bdev_open(bdev_t *dev);

#endif
