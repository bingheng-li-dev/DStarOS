/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _KMALLOC_H_
#define _KMALLOC_H_

#include <stddef.h>
#include <stdint.h>

void *kmalloc(uint64_t size);
void kfree(void *ptr);

extern void pmm_init(void);

#endif