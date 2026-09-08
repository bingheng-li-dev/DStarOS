/*
 * diskio.c - VFS/FatFS 磁盘 I/O 层
 *
 * QEMU 平台：使用内存 ramdisk，落在 memtype.h 划出的 rootfs 预留区上
 *            （ROOTFS_PHYS_BASE，PMM 页帧池之外），内容由 QEMU 的 -device loader
 *            在启动前原样写进去；没装载镜像时那块是零，fatfs_mount 会退回 f_mkfs。
 * VF2 平台：同一条路径，改由 U-Boot 把镜像预载到同一个物理地址，内核侧不用区别对待。
 */

#include "diskio.h"
#include "stringops.h"
#include "memtype.h"

/* ================================================================
 * QEMU Ramdisk 实现
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
    ramdisk_buf = (unsigned char *)pa_to_kva(ROOTFS_PHYS_BASE);
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
