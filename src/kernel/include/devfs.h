#ifndef _DEVFS_H_
#define _DEVFS_H_

/* 注册 devfs 文件系统类型。须在 fs_init() 里、"/dev" 目录已经在根文件系统上
 * 创建之后，调 vfs_mount("/dev", "devfs", NULL) 之前调用。 */
void devfs_register(void);

#endif
