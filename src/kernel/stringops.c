/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "stringops.h"

/**
 * @brief 复制 len 字节；实现上也处理重叠，任一指针为 NULL 时返回 NULL
 */
void *memcpy(void *dst, const void *src, size_t len)
{
    if (NULL == dst || NULL == src)
    {
        return NULL;
    }

    void *ret = dst;

    if (dst <= src || (char *)dst >= (char *)src + len)
    {
        //没有内存重叠，从低地址开始复制
        while (len--)
        {
            *(char *)dst = *(char *)src;
            dst = (char *)dst + 1;
            src = (char *)src + 1;
        }
    }
    else
    {
        //有内存重叠，从高地址开始复制
        src = (char *)src + len - 1;
        dst = (char *)dst + len - 1;
        while (len--)
        {
            *(char *)dst = *(char *)src;
            dst = (char *)dst - 1;
            src = (char *)src - 1;
        }
    }
    return ret;
}

/**
 * @brief 把 s 起的 n 字节填成 c
 */
void *memset(void *s, int c, size_t n)
{
    const unsigned char uc = c;
    unsigned char *su;
    for (su = s; 0 < n; ++su, --n)
        *su = uc;
    return s;
}

/**
 * @brief 复制 n 字节，src 与 dst 允许重叠；返回 dst
 */
void *memmove(void *dst, const void *src, size_t n)
{
    const char *s = src;
    char *d = dst;
    if (s < d && s + n > d)
    {
        s += n, d += n;
        while (n-- > 0)
        {
            *--d = *--s;
        }
    }
    else
    {
        while (n-- > 0)
        {
            *d++ = *s++;
        }
    }
    return dst;
}

/**
 * @brief 按无符号字节比较前 n 字节
 */
int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p = s1, *q = s2;
    while (n-- > 0)
    {
        if (*p != *q)
        {
            return (int)*p - (int)*q;
        }
        p++, q++;
    }
    return 0;
}

/**
 * @brief 比较至多 n 个字符
 */
int strncmp(const char *p, const char *q, uint32_t n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (uint8_t)*p - (uint8_t)*q;
}

/**
 * @brief 查找字符 c 首次出现的位置，找不到返回 NULL（不匹配结尾的 '\0'，与标准库不同）
 */
char *strchr(const char *s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return (char *)s;
    return 0;
}

/**
 * @brief 字符串长度
 */
size_t strlen(const char *s)
{
    size_t n;

    for (n = 0; s[n]; n++)
        ;
    return n;
}
