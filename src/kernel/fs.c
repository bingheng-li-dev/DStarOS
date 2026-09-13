#include "fs.h"
#include "vfs.h"
#include "fatfs_vfs.h"
#include "devfs.h"
#include "errorcode.h"
#include "console.h"
#include "debug.h"
#include "kmalloc.h"

#if defined(VF2) && DEBUG_SDMMC_PROBE
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
 * @brief 端到端校验 SD 读通路：经 VFS → FatFS → diskio → sdmmc 读完整个文件并算 CRC32
 * @param[in] path 要校验的文件
 * @details 板上 BusyBox 裁剪得只剩 cat / ls，没有 md5sum / cmp，只能内核自己算。
 *   4 MB 的文件要连续读 8000 多个块，任何一位读错 CRC 都对不上，比核对签名严格得多。
 *   算法与 zlib.crc32 相同，宿主机上对同一个文件算出的值就是标准答案。
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
    kfree(buf);
    vfs_close(f);
    printf("sdcheck: %s size=%lu crc32=%08x\n", path, (unsigned long)total, crc ^ 0xffffffffU);
}
#endif

/**
 * @note 不加 vfs_lock()——此函数运行在 proc_init() 之前的单核启动阶段，此时还没有任何
 *   pcb（含 idle/init）存在，而 vfs_lock() 内部的 sem_down() 无条件调用
 *   proc_get_current()，此刻拿到的是无效指针，会直接触发缺页异常。这个阶段本来就是
 *   单线程执行，没有并发可言，不需要加锁。
 */
void fs_init(void)
{
    /* 1. 初始化 VFS 全局数据结构（含 vfs_big_lock 本身的初始化）*/
    vfs_init();

    /* 2. 注册 fatfs 文件系统类型 */
    fatfs_register();

    /* 3. 挂载根文件系统（驱动号 0 对应内存 ramdisk，见 diskio.c）*/
    int ret = vfs_mount("/", "fatfs", NULL);
    if (ret != ENO0_NO_ERROR)
    {
        printf("fs_init: root mount failed, err=%d\n", ret);
        return;
    }

    printf("fs_init: root filesystem mounted (fatfs/ramdisk)\n");

    /* 4. devfs：/dev 挂载点本身必须先在根文件系统（FAT）上建好目录，
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
    /* 5. SD 卡挂到 /sd，根仍是 RAM 盘：SD 驱动或卡出问题时系统照样进得了 shell。
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
    fs_verify_file_crc32("/sd/rootfs.img");
    fs_verify_file_crc32("/sd/copy.img");
#endif
#endif
}
