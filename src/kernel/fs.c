/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "fs.h"
#include "vfs.h"
#include "fatfs_vfs.h"
#include "devfs.h"
#include "errorcode.h"
#include "console.h"
#include "debug.h"
#include "bdev.h"
#include "bio.h"
#include "ramdisk.h"
#include "sdcard.h"
#include "probes.h"

/**
 * @brief 挂载启动时的全部文件系统：/ = RAM 盘 FAT，/dev = devfs，VF2 另挂 /sd
 * @note 不加 vfs_lock()：此时在 proc_init() 之前，还没有任何 pcb，而 vfs_lock() 里的
 *   sem_down() 要取当前进程。启动阶段单线程，本来也无需加锁。
 */
void fs_init(void)
{
    /* 块设备与块缓存。注册序号就是 FatFS 的物理驱动器号，必须早于任何挂载 */
    ramdisk_register(0);
#if defined(VF2)
    sdcard_register(1);
#endif
    bio_init();

    vfs_init();

    fatfs_register();

    /* data 为 NULL 即 0 号驱动器（RAM 盘） */
    int ret = vfs_mount("/", "fatfs", NULL);
    if (ret != ENO0_NO_ERROR)
    {
        printf("fs_init: root mount failed, err=%d\n", ret);
        return;
    }

    printf("fs_init: root filesystem mounted (fatfs/ramdisk)\n");

    /* devfs：/dev 挂载点本身必须先在根文件系统（FAT）上建好目录，
     * vfs_mount() 对非 "/" 目标要求挂载点已存在且是目录（见 vfs.c）。 */
    devfs_register();
    ret = vfs_mkdir("/dev", 0755);
    if (ret != ENO0_NO_ERROR)
    {
        printf("fs_init: mkdir /dev failed, err=%d\n", ret);
        return;
    }
    ret = vfs_mount("/dev", "devfs", NULL);
    if (ret != ENO0_NO_ERROR)
    {
        printf("fs_init: devfs mount failed, err=%d\n", ret);
        return;
    }

#if defined(VF2)
    /* SD 卡挂到 /sd，根仍是 RAM 盘：SD 驱动或卡出问题时系统照样进得了 shell。
     * 失败只打印、不中断启动。 */
    ret = vfs_mkdir("/sd", 0755);
    if (ret != ENO0_NO_ERROR && ret != ENO7_EXISTS)
    {
        printf("fs_init: mkdir /sd failed, err=%d\n", ret);
        return;
    }
    ret = vfs_mount("/sd", "fatfs", "1");
    if (ret != ENO0_NO_ERROR)
    {
        printf("fs_init: sd card mount failed, err=%d\n", ret);
        return;
    }
    printf("fs_init: sd card mounted at /sd\n");
#if DEBUG_SDMMC_PROBE
    sdmmc_fs_probe();
#endif
#endif
}
