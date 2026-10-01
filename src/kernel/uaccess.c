/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "stringops.h"
#include "uaccess.h"
#include "errorcode.h"

/**
 * @brief 从用户空间拷贝 n 字节到内核缓冲区
 * @note 只在拷贝期间置位 sstatus.SUM；不做地址校验。
 */
int copy_from_user(void *kdst, const void *usrc, uint64_t n)
{
    unsigned long prev = user_access_begin();
    memcpy(kdst, usrc, n);
    user_access_end(prev);
    return 0;
}

/**
 * @brief 从内核缓冲区拷贝 n 字节到用户空间
 * @note 同 copy_from_user()。
 */
int copy_to_user(void *udst, const void *ksrc, uint64_t n)
{
    unsigned long prev = user_access_begin();
    memcpy(udst, ksrc, n);
    user_access_end(prev);
    return 0;
}

/**
 * @brief 从用户空间拷贝一个以 '\0' 结尾的字符串（参数与返回值见 uaccess.h）
 */
long strncpy_from_user(char *kdst, const char *usrc, size_t n)
{
    if (usrc == NULL || (uint64_t)usrc >= USER_STACK_TOP)
    {
        return ENO8_NULL_POINTER;
    }

    size_t i;
    for (i = 0; i < n; i++)
    {
        if (copy_from_user(&kdst[i], usrc + i, 1) != 0)
        {
            return ENO8_NULL_POINTER;
        }
        if (kdst[i] == '\0')
        {
            return (long)i;
        }
    }
    return ENO11_NAME_TOO_LONG;
}
