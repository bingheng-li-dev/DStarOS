#ifndef _FATFS_VFS_H_
#define _FATFS_VFS_H_

#include "vfs.h"

/* 将 fatfs 文件系统类型注册到 VFS */
void fatfs_register(void);

/* fatfs 文件系统类型描述符（file_system_type_t 实例）*/
extern file_system_type_t fatfs_fs_type;

#endif /* _FATFS_VFS_H_ */
