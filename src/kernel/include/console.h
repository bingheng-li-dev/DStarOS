#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include <stdbool.h>

#include "tinyprintf.h"

/* 前向声明：完整类型在 vfs.h。此处只前向声明，避免把 vfs.h 拉进被广泛 include 的 console.h。 */
struct file;
typedef struct file file_t;

void console_init(void);
void printf(char *fmt, ...);
void panic_impl(const char *func, int line, char *s, ...) __attribute__((noreturn));

/* 造一个内核虚构的 console 设备 file（stdin/stdout/stderr 的后端）。
 * 尚无 /dev/console 设备节点，故直接手搓：不走 vfs_open、无 inode，f_op 直连串口。
 * f_count 初始为 1，装入多个 fd 时由调用方按需 ++。分配失败返回 NULL。 */
file_t *console_open_file(void);

/* 调用点展开 __FUNCTION__ 和 __LINE__，再转发给 panic_impl */
#define panic(s, ...) panic_impl(__FUNCTION__, __LINE__, s, ##__VA_ARGS__)

#endif