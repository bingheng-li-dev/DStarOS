#ifndef _STRINGOPS_H_
#define _STRINGOPS_H_

#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t len);
void *memset(void *s, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int strncmp(const char *p, const char *q, uint32_t n);
char *strchr(const char *s, char c);
void snstr(char *dst, uint16_t const *src, int len);
int wcsncmp(uint16_t const *s1, uint16_t const *s2, int len);
int strlen(const char *s);
void wnstr(uint16_t *dst, char const *src, int len);
char *strncpy(char *s, const char *t, int n);

#endif