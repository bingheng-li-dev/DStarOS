/*
 * diskio.c - VFS/FatFS 磁盘 I/O 层
 *
 * QEMU 平台：使用内存 ramdisk（2MB，4096 个 512 字节扇区），
 *            存放在 BSS 段，内核启动时由 bss_init() 清零。
 * K210 平台：使用 SD 卡驱动（通过 sdcard.h 接口）。
 */

#include "diskio.h"
#include "stringops.h"

#ifdef QEMU

/* ================================================================
 * QEMU Ramdisk 实现
 * ================================================================ */

#define RAMDISK_SECTOR_SIZE    512
#define RAMDISK_SECTOR_COUNT   4096   /* 共 2MB */

/* ramdisk 数据区（位于 BSS 段，内核启动时由 bss_init() 清零）*/
static unsigned char ramdisk_buf[RAMDISK_SECTOR_SIZE * RAMDISK_SECTOR_COUNT];

/* ramdisk 初始化状态标志 */
static int ramdisk_initialized = 0;

/*
 * disk_initialize - 初始化磁盘驱动
 * @pdrv: 物理驱动器编号（仅支持 0）
 * 返回：0 表示就绪；STA_NOINIT 表示失败
 */
DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NOINIT;
    }
    ramdisk_initialized = 1;
    return 0;
}

/*
 * disk_status - 获取磁盘驱动器状态
 * @pdrv: 物理驱动器编号
 * 返回：0 表示就绪；STA_NOINIT 表示未初始化
 */
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NOINIT;
    }
    return ramdisk_initialized ? 0 : STA_NOINIT;
}

/*
 * disk_read - 从 ramdisk 读取扇区数据
 * @pdrv:   物理驱动器编号
 * @buff:   读取数据的目标缓冲区
 * @sector: 起始扇区地址（LBA）
 * @count:  读取的扇区数量
 */
DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || !ramdisk_initialized)
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

/*
 * disk_write - 向 ramdisk 写入扇区数据
 * @pdrv:   物理驱动器编号
 * @buff:   待写入数据的源缓冲区
 * @sector: 起始扇区地址（LBA）
 * @count:  写入的扇区数量
 */
DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || !ramdisk_initialized)
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

/*
 * disk_ioctl - 磁盘设备控制
 * @pdrv: 物理驱动器编号
 * @cmd:  控制命令
 * @buff: 命令参数缓冲区
 */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
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

/* ================================================================
 * 导出 ramdisk 扇区总数供 fatfs_vfs.c 使用
 * ================================================================ */
unsigned int ramdisk_get_sector_count(void)
{
    return RAMDISK_SECTOR_COUNT;
}

#else  /* !QEMU —— K210 SD 卡实现 */

/* ================================================================
 * K210 SD 卡实现
 * ================================================================ */

#include "sdcard.h"

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NOINIT;
    }
    if (sdcard_init() != 0)
    {
        return STA_NOINIT;
    }
    return 0;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NOINIT;
    }
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    if (sdcard_read_sector_dma(buff, sector, count) != 0)
    {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    if (sdcard_write_sector_dma((BYTE *)buff, sector, count) != 0)
    {
        return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    switch (cmd)
    {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buff = sdcard_get_sector_count();
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

unsigned int ramdisk_get_sector_count(void)
{
    return (unsigned int)sdcard_get_sector_count();
}

#endif /* QEMU */
