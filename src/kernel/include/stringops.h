/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _STRINGOPS_H_
#define _STRINGOPS_H_

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *s, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
int strncmp(const char *p, const char *q, uint32_t n);
char *strchr(const char *s, char c);
size_t strlen(const char *s);

#endif