#include "fs.h"
#include "vfs.h"
#include "fatfs_vfs.h"
#include "devfs.h"
#include "errorcode.h"
#include "console.h"

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
}
