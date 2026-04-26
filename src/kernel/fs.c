#include "fs.h"
#include "vfs.h"
#include "fatfs_vfs.h"
#include "errorcode.h"
#include "console.h"

void fs_init(void)
{
    /* 1. 初始化 VFS 全局数据结构 */
    vfs_init();

    /* 2. 注册 fatfs 文件系统类型 */
    fatfs_register();

    /* 3. 挂载根文件系统（驱动号 0 对应 QEMU ramdisk 或 K210 SD 卡）*/
    int ret = vfs_mount("/", "fatfs", NULL);
    if (ret != ENO0_NO_ERROR)
    {
        printf("fs_init: root mount failed, err=%d\n", ret);
        return;
    }
    printf("fs_init: root filesystem mounted (fatfs/ramdisk)\n");
}
