/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "stringops.h"

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

void *memset(void *s, int c, size_t n)
{
    const unsigned char uc = c;
    unsigned char *su;
    for (su = s; 0 < n; ++su, --n)
        *su = uc;
    return s;
}

/* *
 * memmove - copies the values of @n bytes from the location pointed by @src to
 * the memory area pointed by @dst. @src and @dst are allowed to overlap.
 * @dst     pointer to the destination array where the content is to be copied
 * @src     pointer to the source of data to by copied
 * @n:      number of bytes to copy
 *
 * The memmove() function returns @dst.
 * 
 * 比memcpy更安全：memmove在copy两个有重叠区域的内存时可以保证copy的正确
 * 
 * */
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

int strncmp(const char *p, const char *q, uint32_t n)
{
    while (n > 0 && *p && *p == *q)
        n--, p++, q++;
    if (n == 0)
        return 0;
    return (uint8_t)*p - (uint8_t)*q;
}

char *strchr(const char *s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return (char *)s;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n;

    for (n = 0; s[n]; n++)
        ;
    return n;
}
