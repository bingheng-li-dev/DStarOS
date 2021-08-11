#ifndef _STRINGOPS_H_
#define _STRINGOPS_H_

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *s, int c, size_t n);

#endif