/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "fs.h"
#include "vfs.h"
#include "fatfs_vfs.h"
#include "devfs.h"
#include "errorcode.h"
#include "console.h"
#include "debug.h"
#include "kmalloc.h"
#include "bdev.h"
#include "bio.h"
#include "ramdisk.h"
#include "sdcard.h"
#include "ktime.h"

#if defined(VF2) && DEBUG_SDMMC_PROBE
#include "sdmmc.h"

#define FS_PATTERN_FILE_SIZE (2U * 1024 * 1024)

/**
 * @brief 按 zlib.crc32 的算法累加 CRC32（反射多项式 0xEDB88320，逐位计算）
 * @param[in] crc 当前累加值（首次传 0xffffffff）
 * @param[in] p   数据
 * @param[in] n   字节数
 * @return 更新后的累加值（最终结果需再异或 0xffffffff）
 */
static uint32_t fs_crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
        {
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return crc;
}

/**
 * @brief 端到端校验 SD 读通路：经 VFS → FatFS → 块缓存 → 驱动读完整个文件并算 CRC32
 * @param[in] path 要校验的文件
 * @details 板上 BusyBox 没有 md5sum / cmp，只能内核自己算；算法与 zlib.crc32 相同，
 *   宿主机上对同一个文件算出的值就是标准答案。同时打印耗时与这一趟里块缓存的命中 / 未命中数，
 *   同一文件连续校验两次即可看出缓存是否生效。
 * @note 运行在 fs_init() 里，同样不加 vfs_lock()（原因见 fs_init 的注释）。
 */
static void fs_verify_file_crc32(const char *path)
{
    file_t *f = vfs_open(path, O_RDONLY, NULL);
    if (f == NULL)
    {
        printf("sdcheck: cannot open %s\n", path);
        return;
    }
    uint8_t *buf = (uint8_t *)kmalloc(4096);
    if (buf == NULL)
    {
        vfs_close(f);
        printf("sdcheck: no memory\n");
        return;
    }

    bio_stats_t before;
    bio_get_stats(&before);
    sdmmc_stats_t sd_before;
    sdmmc_get_stats(&sd_before);
    uint64_t t0 = ktime_get_ns();

    uint32_t crc = 0xffffffffU;
    uint64_t total = 0;
    for (;;)
    {
        ssize_t n = vfs_read(f, buf, 4096);
        if (n < 0)
        {
            printf("sdcheck: read error %ld at offset %lu\n", (long)n, (unsigned long)total);
            break;
        }
        if (n == 0)
        {
            break;
        }
        crc = fs_crc32_update(crc, buf, (size_t)n);
        total += (uint64_t)n;
    }

    uint64_t ms = (ktime_get_ns() - t0) / 1000000;
    bio_stats_t after;
    bio_get_stats(&after);
    sdmmc_stats_t sd_after;
    sdmmc_get_stats(&sd_after);
    kfree(buf);
    vfs_close(f);
    printf("sdcheck: %s size=%lu crc32=%08x time=%lums cache hits=%lu misses=%lu dev_reads=%lu cmd17=%lu cmd18=%lu\n",
           path, (unsigned long)total, crc ^ 0xffffffffU, (unsigned long)ms,
           (unsigned long)(after.hits - before.hits), (unsigned long)(after.misses - before.misses),
           (unsigned long)(after.dev_reads - before.dev_reads),
           (unsigned long)(sd_after.cmd17 - sd_before.cmd17), (unsigned long)(sd_after.cmd18 - sd_before.cmd18));
}

/**
 * @brief 写通路对比：经 VFS 截断重写一个 2 MB 的确定图案文件，打印写耗时与实际发出的写命令数
 * @param[in] path 目标文件
 * @details 每次写 4 KB，FatFS 把整扇区部分一次交给 disk_write（8 块）：单块模式下是 8 条 CMD24，
 *   多块模式下是 1 条 CMD25。耗时只计 vfs_write 与 vfs_close，不含生成图案和算 CRC。
 *   打印的 CRC 是写入内容的 CRC；下次开机 fs_verify_file_crc32() 冷读同一文件应得到同一个值，
 *   以此确认数据真的落了盘。
 */
static void fs_write_pattern_file(const char *path)
{
    int err = 0;
    file_t *f = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &err);
    if (f == NULL)
    {
        printf("sdwrite: cannot create %s, err=%d\n", path, err);
        return;
    }
    uint8_t *buf = (uint8_t *)kmalloc(4096);
    if (buf == NULL)
    {
        vfs_close(f);
        printf("sdwrite: no memory\n");
        return;
    }

    sdmmc_stats_t before;
    sdmmc_get_stats(&before);
    uint64_t write_ns = 0;
    uint32_t crc = 0xffffffffU;
    uint64_t total = 0;
    while (total < FS_PATTERN_FILE_SIZE)
    {
        for (uint32_t i = 0; i < 4096; i++)
        {
            uint64_t x = total + i;
            buf[i] = (uint8_t)(x ^ (x >> 7) ^ (x >> 13));
        }
        uint64_t t = ktime_get_ns();
        ssize_t n = vfs_write(f, buf, 4096);
        write_ns += ktime_get_ns() - t;
        if (n != 4096)
        {
            printf("sdwrite: write error %ld at offset %lu\n", (long)n, (unsigned long)total);
            break;
        }
        crc = fs_crc32_update(crc, buf, 4096);
        total += 4096;
    }
    uint64_t t = ktime_get_ns();
    vfs_close(f);
    write_ns += ktime_get_ns() - t;
    kfree(buf);

    sdmmc_stats_t after;
    sdmmc_get_stats(&after);
    printf("sdwrite: %s size=%lu crc32=%08x time=%lums cmd24=%lu cmd25=%lu blocks=%lu cmd12_manual=%lu\n",
           path, (unsigned long)total, crc ^ 0xffffffffU, (unsigned long)(write_ns / 1000000),
           (unsigned long)(after.cmd24 - before.cmd24), (unsigned long)(after.cmd25 - before.cmd25),
           (unsigned long)(after.blocks_written - before.blocks_written),
           (unsigned long)(after.cmd12_manual - before.cmd12_manual));
}
#endif

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
    /* copy.img 与 rootfs.img 内容相同、各占不同的块：分别以逐块 / 多块冷读，再读一遍看缓存 */
    sdmmc_set_multiblock(false);
    fs_verify_file_crc32("/sd/copy.img");
    sdmmc_set_multiblock(true);
    fs_verify_file_crc32("/sd/rootfs.img");
    fs_verify_file_crc32("/sd/rootfs.img");
    /* 先冷读上次开机写下的文件确认落盘，再分别以逐块 / 多块重写 */
    fs_verify_file_crc32("/sd/wsingle.bin");
    fs_verify_file_crc32("/sd/wmulti.bin");
    sdmmc_set_multiblock(false);
    fs_write_pattern_file("/sd/wsingle.bin");
    sdmmc_set_multiblock(true);
    fs_write_pattern_file("/sd/wmulti.bin");
#endif
#endif
}
