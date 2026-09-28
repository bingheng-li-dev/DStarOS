/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include <stdbool.h>

#include "tinyprintf.h"
#include "sync.h"

struct file;
typedef struct file file_t;

extern osslock_t ConsoleLock;

void console_init(void);
void printf(char *fmt, ...);
void panic_impl(const char *func, int line, char *s, ...) __attribute__((noreturn));

/* @deprecated 转调 tty_open_file()（tty.h），保留只是为了不用改调用点。
 * 新代码直接调 tty_open_file()。 */
file_t *console_open_file(void);

/* 调用点展开 __FUNCTION__ 和 __LINE__，再转发给 panic_impl */
#define panic(s, ...) panic_impl(__FUNCTION__, __LINE__, s, ##__VA_ARGS__)

#endif /* _CONSOLE_H_ */
