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

/* convert wide char string into uchar string */
void snstr(char *dst, uint16_t const *src, int len)
{
    while (len-- && *src)
    {
        *dst++ = (uint8_t)(*src & 0xff);
        src++;
    }
    while (len-- > 0)
        *dst++ = 0;
}

int wcsncmp(uint16_t const *s1, uint16_t const *s2, int len)
{
    int ret = 0;

    while (len-- && *s1)
    {
        ret = (int)(*s1++ - *s2++);
        if (ret)
            break;
    }

    return ret;
}

int strlen(const char *s)
{
    int n;

    for (n = 0; s[n]; n++)
        ;
    return n;
}

/* convert uchar string into wide char string */
void wnstr(uint16_t *dst, char const *src, int len)
{
    while (len-- && *src)
    {
        *(uint8_t *)dst = *src++;
        dst++;
    }

    *dst = 0;
}

char *strncpy(char *s, const char *t, int n)
{
    char *os;

    os = s;
    while (n-- > 0 && (*s++ = *t++) != 0)
        ;
    while (n-- > 0)
        *s++ = 0;
    return os;
}