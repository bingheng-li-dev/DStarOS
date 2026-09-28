/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _FATFS_VFS_H_
#define _FATFS_VFS_H_

#include "vfs.h"

/* 将 fatfs 文件系统类型注册到 VFS */
void fatfs_register(void);

#endif /* _FATFS_VFS_H_ */
