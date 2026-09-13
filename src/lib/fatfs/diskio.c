/*
 * diskio.c - VFS/FatFS 磁盘 I/O 层
 *
 * 0 号驱动器（两个平台）：内存 ramdisk，落在 memtype.h 划出的 rootfs 预留区上
 *            （ROOTFS_PHYS_BASE，PMM 页帧池之外）。QEMU 由 -device loader、VF2 由 U-Boot
 *            在启动前把镜像原样写进去；没装载镜像时那块是零，fatfs_mount 会退回 f_mkfs。
 * 1 号驱动器（仅 VF2）：SD 卡第一个分区，**只读**。经 sdmmc 驱动按块读，
 *            扇区号加上分区起始 LBA 后下发——FatFS 看到的是"0 号扇区就是 FAT 引导扇区"的盘。
 */

#include "diskio.h"
#include "stringops.h"
#include "memtype.h"

#if defined(VF2)
#include "sdmmc.h"
#include "console.h"
#include "errorcode.h"
#endif

#define RAMDISK_PDRV           0
#define SDCARD_PDRV            1

/* ================================================================
 * 0 号驱动器：Ramdisk
 * ================================================================ */

#define RAMDISK_SECTOR_SIZE    512
/* 整个预留区都当成这块"盘"。**不必等于镜像大小**：f_mount 读的是引导扇区里记的
 * 总扇区数（镜像自己说了算），这个数只被 f_mkfs 用来决定格式化多大。
 * 于是镜像可以比预留区小，剩下的空间留着以后放大。 */
#define RAMDISK_SECTOR_COUNT   (ROOTFS_MAX_SIZE / RAMDISK_SECTOR_SIZE)

/* ramdisk 数据区：指向 rootfs 预留区的内核虚拟地址。
 * 不能写成静态初始化——pa_to_kva() 是内联函数、不是常量表达式，而且这块地址
 * 只有在 MMU 打开、内核偏移映射建好之后才可访问（KERNEL_MAP_END 覆盖了它）。
 * disk_initialize() 由 fatfs_mount() 调用，那时 os_init_after_mmu_enable 早就跑完了。 */
static unsigned char *ramdisk_buf;

/* ramdisk 初始化状态标志 */
static int ramdisk_initialized = 0;

#if defined(VF2)
/* ================================================================
 * 1 号驱动器：SD 卡第一个分区（只读）
 * ================================================================ */

static uint64_t sdcard_part_start;
static DSTATUS sdcard_stat = STA_NOINIT;
static uint8_t sdcard_sector[512];

static inline uint32_t sdcard_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t sdcard_le64(const uint8_t *p)
{
    return (uint64_t)sdcard_le32(p) | ((uint64_t)sdcard_le32(p + 4) << 32);
}

/**
 * @brief 找 SD 卡第一个分区的起始 LBA
 * @param[out] start 分区起始块号
 * @retval ENO0_NO_ERROR 找到
 * @retval ENO13_NO_FS   LBA0 不是有效的 MBR / GPT，或第一个分区为空
 * @return 其余为 sdmmc_read_blocks() 的错误码
 * @details 自己解析分区表，而不是交给 FatFS：FatFS R0.11 只认 MBR，遇到 GPT 的保护性 MBR
 *   会报"没有文件系统"。0xee 表示 GPT，取 GPT 头里的分区表位置，再取第一项的起始 LBA。
 * @note 只支持第一个分区。U-Boot 的 `fatls mmc 1:1` 能列出内容，说明本板的卡满足这一点。
 */
static int sdcard_find_first_partition(uint64_t *start)
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
    return (*start != 0) ? ENO0_NO_ERROR : ENO13_NO_FS;
}

/**
 * @brief 初始化 1 号驱动器：接手 SD 卡并定位第一个分区
 * @return STA_PROTECT 表示可读（只读）；STA_NOINIT 表示失败
 * @note 幂等。FatFS 首次挂载时 find_volume 自己也会调 disk_initialize，
 *   而 fatfs_mount_cb 已经先调过一次——不短路的话会重复接手卡、重复解析分区表
 *   （板上实测分区信息打印了两次）。
 */
static DSTATUS sdcard_initialize(void)
{
    if (!(sdcard_stat & STA_NOINIT))
    {
        return sdcard_stat;
    }

    int ret = sdmmc_init();
    if (ret != ENO0_NO_ERROR)
    {
        printf("sdcard: controller/card not ready, err=%d\n", ret);
        sdcard_stat = STA_NOINIT;
        return sdcard_stat;
    }
    ret = sdcard_find_first_partition(&sdcard_part_start);
    if (ret != ENO0_NO_ERROR)
    {
        printf("sdcard: no usable partition, err=%d\n", ret);
        sdcard_stat = STA_NOINIT;
        return sdcard_stat;
    }
    printf("sdcard: partition 1 at lba %lu\n", (unsigned long)sdcard_part_start);
    sdcard_stat = STA_PROTECT;
    return sdcard_stat;
}
#endif

/*
 * disk_initialize - 初始化磁盘驱动
 * @pdrv: 物理驱动器编号（0 = ramdisk；1 = SD 卡，仅 VF2）
 * 返回：0 或 STA_PROTECT 表示就绪；STA_NOINIT 表示失败
 */
DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv == RAMDISK_PDRV)
    {
        ramdisk_buf = (unsigned char *)pa_to_kva(ROOTFS_PHYS_BASE);
        ramdisk_initialized = 1;
        return 0;
    }
#if defined(VF2)
    if (pdrv == SDCARD_PDRV)
    {
        return sdcard_initialize();
    }
#endif
    return STA_NOINIT;
}

/*
 * disk_status - 获取磁盘驱动器状态
 * @pdrv: 物理驱动器编号
 * 返回：0 表示就绪；STA_PROTECT 表示只读就绪；STA_NOINIT 表示未初始化
 */
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv == RAMDISK_PDRV)
    {
        return ramdisk_initialized ? 0 : STA_NOINIT;
    }
#if defined(VF2)
    if (pdrv == SDCARD_PDRV)
    {
        return sdcard_stat;
    }
#endif
    return STA_NOINIT;
}

/*
 * disk_read - 读取扇区数据
 * @pdrv:   物理驱动器编号
 * @buff:   读取数据的目标缓冲区
 * @sector: 起始扇区地址（驱动器内 LBA；SD 卡为分区内 LBA）
 * @count:  读取的扇区数量
 */
DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv == RAMDISK_PDRV)
    {
        if (!ramdisk_initialized)
        {
            return RES_NOTRDY;
        }
        if (sector + count > RAMDISK_SECTOR_COUNT)
        {
            return RES_PARERR;
        }
        memcpy(buff,
               ramdisk_buf + sector * RAMDISK_SECTOR_SIZE,
               (unsigned long)count * RAMDISK_SECTOR_SIZE);
        return RES_OK;
    }
#if defined(VF2)
    if (pdrv == SDCARD_PDRV)
    {
        if (sdcard_stat & STA_NOINIT)
        {
            return RES_NOTRDY;
        }
        return (sdmmc_read_blocks(sdcard_part_start + sector, buff, count) == ENO0_NO_ERROR)
               ? RES_OK : RES_ERROR;
    }
#endif
    return RES_PARERR;
}

/*
 * disk_write - 写入扇区数据
 * @pdrv:   物理驱动器编号
 * @buff:   待写入数据的源缓冲区
 * @sector: 起始扇区地址（LBA）
 * @count:  写入的扇区数量
 * 注：SD 卡当前只读，一律返回 RES_WRPRT。
 */
DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv == RAMDISK_PDRV)
    {
        if (!ramdisk_initialized)
        {
            return RES_NOTRDY;
        }
        if (sector + count > RAMDISK_SECTOR_COUNT)
        {
            return RES_PARERR;
        }
        memcpy(ramdisk_buf + sector * RAMDISK_SECTOR_SIZE,
               buff,
               (unsigned long)count * RAMDISK_SECTOR_SIZE);
        return RES_OK;
    }
#if defined(VF2)
    if (pdrv == SDCARD_PDRV)
    {
        return RES_WRPRT;
    }
#endif
    return RES_PARERR;
}

/*
 * disk_ioctl - 磁盘设备控制
 * @pdrv: 物理驱动器编号
 * @cmd:  控制命令
 * @buff: 命令参数缓冲区
 */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv == RAMDISK_PDRV)
    {
        switch (cmd)
        {
        case CTRL_SYNC:
            /* ramdisk 无需同步 */
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(DWORD *)buff = RAMDISK_SECTOR_COUNT;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = RAMDISK_SECTOR_SIZE;
            return RES_OK;
        case GET_BLOCK_SIZE:
            /* ramdisk 的擦除块大小为 1 个扇区 */
            *(DWORD *)buff = 1;
            return RES_OK;
        default:
            return RES_PARERR;
        }
    }
#if defined(VF2)
    if (pdrv == SDCARD_PDRV)
    {
        /* 只读挂载用不到扇区数与擦除块（那是 f_mkfs / f_getfree 的事），不提供 */
        return (cmd == CTRL_SYNC) ? RES_OK : RES_PARERR;
    }
#endif
    return RES_PARERR;
}
