/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _CONTAINEROF_H_
#define _CONTAINEROF_H_

/*
 * ContainerOf宏。
 * @param ptr: 指向成员的指针。
 * @param type: 成员所嵌入的容器结构体类型。
 * @param member: 结构体中的成员名。
 */
#define getContainer(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))

#endif
