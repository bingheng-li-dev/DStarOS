/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "bdev.h"
#include "errorcode.h"

static bdev_t *bdev_table[BDEV_MAX];

/**
 * @brief 注册一个块设备
 * @param[in] dev 设备描述符，须在整个系统生命周期内有效（驱动里的静态对象）
 * @param[in] id  注册序号，同时就是 FatFS 的物理驱动器号
 * @retval ENO0_NO_ERROR    注册成功
 * @retval ENO6_INVAL_PARAM 序号越界或 dev / ops 为空
 * @retval ENO7_EXISTS      该序号已被占用
 * @note 只在 fs_init() 挂载任何文件系统之前调用，启动阶段单线程，不加锁。
 */
int bdev_register(bdev_t *dev, uint32_t id)
{
    if (id >= BDEV_MAX || dev == NULL || dev->ops == NULL)
    {
        return ENO6_INVAL_PARAM;
    }
    if (bdev_table[id] != NULL)
    {
        return ENO7_EXISTS;
    }
    dev->id = id;
    dev->ready = false;
    bdev_table[id] = dev;
    return ENO0_NO_ERROR;
}

/**
 * @brief 按注册序号取块设备
 * @param[in] id 注册序号
 * @return 设备描述符；未注册返回 NULL
 */
bdev_t *bdev_get(uint32_t id)
{
    return (id < BDEV_MAX) ? bdev_table[id] : NULL;
}

/**
 * @brief 确保设备已初始化，幂等
 * @param[in] dev 设备描述符
 * @retval ENO0_NO_ERROR 设备就绪（本次初始化成功，或此前已就绪）
 * @return 其余为驱动 init 回调的错误码
 * @details FatFS 首次挂载时 find_volume 自己会调 disk_initialize，而挂载回调已经先调过一次；
 *   幂等放在这一层，驱动不必各自防重入。
 */
int bdev_open(bdev_t *dev)
{
    if (dev->ready)
    {
        return ENO0_NO_ERROR;
    }
    int ret = dev->ops->init(dev);
    if (ret == ENO0_NO_ERROR)
    {
        dev->ready = true;
    }
    return ret;
}
