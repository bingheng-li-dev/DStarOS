/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/*
 * diskio.c - FatFS 磁盘 I/O 层
 *
 * 只做一层转发：FatFS 的物理驱动器号就是块设备的注册序号（见 fs_init()：0 = 内存盘，
 * 1 = VF2 的 SD 卡第一个分区）。越界检查在这里统一做，读写一律经过块缓存（bio.c）。
 */

#include "diskio.h"
#include "bdev.h"
#include "bio.h"
#include "errorcode.h"

/**
 * @brief 设备状态折算成 FatFS 的 DSTATUS
 * @param[in] dev 设备描述符，可为 NULL
 * @return 未注册或未就绪为 STA_NOINIT；只读设备为 STA_PROTECT；否则 0
 */
static DSTATUS diskio_status_of(const bdev_t *dev)
{
    if (dev == NULL || !dev->ready)
    {
        return STA_NOINIT;
    }
    return dev->read_only ? STA_PROTECT : 0;
}

/*
 * disk_initialize - 初始化磁盘驱动（幂等，见 bdev_open）
 * @pdrv: 物理驱动器编号
 * 返回：0 或 STA_PROTECT 表示就绪；STA_NOINIT 表示失败
 */
DSTATUS disk_initialize(BYTE pdrv)
{
    bdev_t *dev = bdev_get(pdrv);
    if (dev == NULL || bdev_open(dev) != ENO0_NO_ERROR)
    {
        return STA_NOINIT;
    }
    return diskio_status_of(dev);
}

/*
 * disk_status - 获取磁盘驱动器状态
 * @pdrv: 物理驱动器编号
 */
DSTATUS disk_status(BYTE pdrv)
{
    return diskio_status_of(bdev_get(pdrv));
}

/*
 * disk_read - 读取扇区数据
 * @pdrv:   物理驱动器编号
 * @buff:   读取数据的目标缓冲区
 * @sector: 起始扇区地址（设备内 LBA）
 * @count:  读取的扇区数量
 */
DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    bdev_t *dev = bdev_get(pdrv);
    if (dev == NULL || !dev->ready)
    {
        return RES_NOTRDY;
    }
    if ((uint64_t)sector + count > dev->nr_blocks)
    {
        return RES_PARERR;
    }
    return (bio_read(dev, sector, buff, count) == ENO0_NO_ERROR) ? RES_OK : RES_ERROR;
}

/*
 * disk_write - 写入扇区数据
 * @pdrv:   物理驱动器编号
 * @buff:   待写入数据的源缓冲区
 * @sector: 起始扇区地址（设备内 LBA）
 * @count:  写入的扇区数量
 * 注：直写（write-through）——返回时数据已在设备上，缓存里没有待落盘的块。
 */
DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    bdev_t *dev = bdev_get(pdrv);
    if (dev == NULL || !dev->ready)
    {
        return RES_NOTRDY;
    }
    if (dev->read_only)
    {
        return RES_WRPRT;
    }
    if ((uint64_t)sector + count > dev->nr_blocks)
    {
        return RES_PARERR;
    }
    return (bio_write(dev, sector, buff, count) == ENO0_NO_ERROR) ? RES_OK : RES_ERROR;
}

/*
 * disk_ioctl - 磁盘设备控制
 * @pdrv: 物理驱动器编号
 * @cmd:  控制命令
 * @buff: 命令参数缓冲区
 */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    bdev_t *dev = bdev_get(pdrv);
    if (dev == NULL || !dev->ready)
    {
        return RES_NOTRDY;
    }
    switch (cmd)
    {
    case CTRL_SYNC:
        /* 直写：每次写都已落到设备，没有需要冲刷的缓冲 */
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buff = (DWORD)dev->nr_blocks;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = BDEV_BLOCK_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
