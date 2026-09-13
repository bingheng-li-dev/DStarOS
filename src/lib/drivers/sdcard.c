#include "sdcard.h"

#if defined(VF2)

#include "bdev.h"
#include "sdmmc.h"
#include "stringops.h"
#include "console.h"
#include "errorcode.h"

static uint64_t sdcard_part_start;
static uint8_t sdcard_sector[BDEV_BLOCK_SIZE];

static inline uint32_t sdcard_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t sdcard_le64(const uint8_t *p)
{
    return (uint64_t)sdcard_le32(p) | ((uint64_t)sdcard_le32(p + 4) << 32);
}

/**
 * @brief 找 SD 卡第一个分区的起始 LBA 与扇区数
 * @param[out] start   分区起始块号
 * @param[out] sectors 分区扇区数
 * @retval ENO0_NO_ERROR 找到
 * @retval ENO13_NO_FS   LBA0 不是有效的 MBR / GPT，或第一个分区为空
 * @return 其余为 sdmmc_read_blocks() 的错误码
 * @details 自己解析分区表，而不是交给 FatFS：FatFS R0.11 只认 MBR，遇到 GPT 的保护性 MBR
 *   会报"没有文件系统"。0xee 表示 GPT，取 GPT 头里的分区表位置，再取第一项。
 * @note 只支持第一个分区。
 */
static int sdcard_find_first_partition(uint64_t *start, uint64_t *sectors)
{
    int ret = sdmmc_read_blocks(0, sdcard_sector, 1);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    if (sdcard_sector[510] != 0x55 || sdcard_sector[511] != 0xaa)
    {
        return ENO13_NO_FS;
    }

    if (sdcard_sector[450] != 0xee)
    {
        *start = sdcard_le32(sdcard_sector + 454);
        *sectors = sdcard_le32(sdcard_sector + 458);
        return (*start != 0) ? ENO0_NO_ERROR : ENO13_NO_FS;
    }

    ret = sdmmc_read_blocks(1, sdcard_sector, 1);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    if (memcmp(sdcard_sector, "EFI PART", 8) != 0)
    {
        return ENO13_NO_FS;
    }
    uint64_t entries_lba = sdcard_le64(sdcard_sector + 72);
    ret = sdmmc_read_blocks(entries_lba, sdcard_sector, 1);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    *start = sdcard_le64(sdcard_sector + 32);
    uint64_t last_lba = sdcard_le64(sdcard_sector + 40);
    *sectors = (last_lba >= *start) ? last_lba - *start + 1 : 0;
    return (*start != 0) ? ENO0_NO_ERROR : ENO13_NO_FS;
}

/**
 * @brief 初始化 SD 分区设备：接手控制器与卡，定位第一个分区
 * @param[in,out] dev 设备描述符，写入分区扇区数
 * @retval ENO0_NO_ERROR 就绪
 * @return 其余为 sdmmc_init() 或分区解析的错误码
 * @note 幂等由 bdev_open() 保证，这里不必防重入。
 */
static int sdcard_init(bdev_t *dev)
{
    int ret = sdmmc_init();
    if (ret != ENO0_NO_ERROR)
    {
        printf("sdcard: controller/card not ready, err=%d\n", ret);
        return ret;
    }
    uint64_t sectors = 0;
    ret = sdcard_find_first_partition(&sdcard_part_start, &sectors);
    if (ret != ENO0_NO_ERROR)
    {
        printf("sdcard: no usable partition, err=%d\n", ret);
        return ret;
    }
    dev->nr_blocks = sectors;
    printf("sdcard: partition 1 at lba %lu, %lu sectors, read-write\n",
           (unsigned long)sdcard_part_start, (unsigned long)sectors);
    return ENO0_NO_ERROR;
}

/**
 * @brief 读分区内若干块
 * @param[in]  dev   设备描述符（未使用）
 * @param[in]  lba   分区内起始块号
 * @param[out] buf   count * 512 字节
 * @param[in]  count 块数
 * @return 同 sdmmc_read_blocks()
 */
static int sdcard_read(bdev_t *dev, uint64_t lba, uint8_t *buf, uint32_t count)
{
    (void)dev;
    return sdmmc_read_blocks(sdcard_part_start + lba, buf, count);
}

/**
 * @brief 写分区内若干块
 * @param[in] dev   设备描述符（未使用）
 * @param[in] lba   分区内起始块号
 * @param[in] buf   count * 512 字节
 * @param[in] count 块数
 * @return 同 sdmmc_write_blocks()
 */
static int sdcard_write(bdev_t *dev, uint64_t lba, const uint8_t *buf, uint32_t count)
{
    (void)dev;
    return sdmmc_write_blocks(sdcard_part_start + lba, buf, count);
}

static const bdev_ops_t sdcard_ops = {
    .init         = sdcard_init,
    .read_blocks  = sdcard_read,
    .write_blocks = sdcard_write,
};

static bdev_t sdcard_dev = {
    .name = "sd0p1",
    .ops  = &sdcard_ops,
};

/**
 * @brief 把 SD 卡第一个分区注册为块设备
 * @param[in] id 注册序号（即 FatFS 物理驱动器号）
 * @return 同 bdev_register()
 */
int sdcard_register(uint32_t id)
{
    return bdev_register(&sdcard_dev, id);
}

#endif
