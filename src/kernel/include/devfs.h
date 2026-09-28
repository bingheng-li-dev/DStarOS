/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _DEVFS_H_
#define _DEVFS_H_

/* 注册 devfs 文件系统类型，须早于 vfs_mount("/dev", "devfs", NULL) */
void devfs_register(void);

#endif /* _DEVFS_H_ */
