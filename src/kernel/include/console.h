#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include <stdbool.h>

#include "tinyprintf.h"

void console_init(void);
void printf(char *fmt, ...);
void panic_impl(const char *func, int line, char *s, ...) __attribute__((noreturn));

/* 调用点展开 __FUNCTION__ 和 __LINE__，再转发给 panic_impl */
#define panic(s, ...) panic_impl(__FUNCTION__, __LINE__, s, ##__VA_ARGS__)

#endif