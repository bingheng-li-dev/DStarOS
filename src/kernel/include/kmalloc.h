#ifndef _KMALLOC_H_
#define _KMALLOC_H_

#include <stddef.h>
#include <stdint.h>

void *kmalloc(uint64_t size);
void kfree(void *ptr);

extern void physicalMemoryManagementInit(void);

#endif