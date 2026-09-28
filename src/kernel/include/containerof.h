/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _CONTAINEROF_H_
#define _CONTAINEROF_H_

/**
 * @brief 由成员指针反推容器结构体指针（同 Linux 的 container_of）
 */
#define getContainer(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))

#endif /* _CONTAINEROF_H_ */
