/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "ramdisk.h"
#include "bdev.h"
#include "memtype.h"
#include "stringops.h"
#include "errorcode.h"

/* 整个 rootfs 预留区都当成这块"盘"。**不必等于镜像大小**：f_mount 读的是引导扇区里记的
 * 总扇区数（镜像自己说了算），这个数只被 f_mkfs 用来决定格式化多大。
 * 于是镜像可以比预留区小，剩下的空间留着以后放大。 */
#define RAMDISK_BLOCK_COUNT (ROOTFS_MAX_SIZE / BDEV_BLOCK_SIZE)

/* 指向 rootfs 预留区的内核虚拟地址。不能静态初始化——pa_to_kva() 不是常量表达式，
 * 而且这块地址只有在 MMU 打开、内核偏移映射建好之后才可访问（KERNEL_MAP_END 覆盖了它）。 */
static uint8_t *ramdisk_base;

/**
 * @brief 初始化内存盘：定位 rootfs 预留区
 * @param[in,out] dev 设备描述符，写入块数
 * @retval ENO0_NO_ERROR 恒成功
 * @note 镜像由 QEMU 的 -device loader 或 U-Boot 在启动前原样写进预留区；没装载镜像时那块是零，
 *   fatfs_mount 会退回 f_mkfs。
 */
static int ramdisk_init(bdev_t *dev)
{
    ramdisk_base = (uint8_t *)pa_to_kva(ROOTFS_PHYS_BASE);
    dev->nr_blocks = RAMDISK_BLOCK_COUNT;
    return ENO0_NO_ERROR;
}

/**
 * @brief 读若干块
 * @param[in]  dev   设备描述符（未使用）
 * @param[in]  lba   起始块号
 * @param[out] buf   count * 512 字节
 * @param[in]  count 块数
 * @retval ENO0_NO_ERROR 恒成功（越界由 diskio 在上层拦截）
 */
static int ramdisk_read(bdev_t *dev, uint64_t lba, uint8_t *buf, uint32_t count)
{
    (void)dev;
    memcpy(buf, ramdisk_base + lba * BDEV_BLOCK_SIZE, (unsigned long)count * BDEV_BLOCK_SIZE);
    return ENO0_NO_ERROR;
}

/**
 * @brief 写若干块
 * @param[in] dev   设备描述符（未使用）
 * @param[in] lba   起始块号
 * @param[in] buf   count * 512 字节
 * @param[in] count 块数
 * @retval ENO0_NO_ERROR 恒成功（越界由 diskio 在上层拦截）
 */
static int ramdisk_write(bdev_t *dev, uint64_t lba, const uint8_t *buf, uint32_t count)
{
    (void)dev;
    memcpy(ramdisk_base + lba * BDEV_BLOCK_SIZE, buf, (unsigned long)count * BDEV_BLOCK_SIZE);
    return ENO0_NO_ERROR;
}

static const bdev_ops_t ramdisk_ops = {
    .init         = ramdisk_init,
    .read_blocks  = ramdisk_read,
    .write_blocks = ramdisk_write,
};

static bdev_t ramdisk_dev = {
    .name = "ram0",
    .ops  = &ramdisk_ops,
};

/**
 * @brief 把内存盘注册为块设备
 * @param[in] id 注册序号（即 FatFS 物理驱动器号）
 * @return 同 bdev_register()
 */
int ramdisk_register(uint32_t id)
{
    return bdev_register(&ramdisk_dev, id);
}
