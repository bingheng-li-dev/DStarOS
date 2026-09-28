/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _TTY_H_
#define _TTY_H_

#include <stdbool.h>

#include "sync.h"      /* osslock_t, waitq_t */
#include "types.h"     /* ssize_t, size_t */
#include "linux_abi.h" /* struct linux_termios, struct winsize */

struct file;
typedef struct file file_t;
struct file_operations;
typedef struct file_operations file_operations_t;

/* TTY 输入环形缓冲大小。一行足够长即可，不需要跟 PIPE_SIZE 对齐——TTY 走的是行规范层，
 * 不要求单次写具备 PIPE_BUF 那种原子性。 */
#define TTY_BUF_SIZE 256

/* 最多同时挂起多少个尚未被 read() 消费的行首 ^D。批量喂入时读者可能远远落后，
 * 连按的每个 ^D 都是流里独立的零宽标记，须在 read_pos 追到各自位置时单独交付，
 * 所以用队列而不是一个标志位（否则后一个会覆盖前一个）。 */
#define TTY_MAX_PENDING_EOF 4

/*
 * TTY 设备核心结构体。全系统一个单例（一个串口即一个终端，不做多终端/pty）。
 * `buf`/`read_pos`/`line_pos`/`edit_pos` 是三索引环形缓冲：
 *   [read_pos, line_pos) 是已提交成行、可被 tty_read() 取走的数据；
 *   [line_pos, edit_pos) 是正在编辑、尚未提交（没按下回车/未触发 raw 模式即时提交）的半行；
 *   三者单调递增，按 `% TTY_BUF_SIZE` 取模访问，不做各自独立回绕。
 */
typedef struct tty
{
    osslock_t lock;               /* 保护本结构体全部字段，也保护 wq_read */
    char      buf[TTY_BUF_SIZE];  /* 静态数组：输入侧在中断上下文，不能动态分配 */
    uint32_t  read_pos;           /* 下一个交给 tty_read() 的字节 */
    uint32_t  line_pos;           /* 已提交成行的末尾 */
    uint32_t  edit_pos;           /* 已敲入的末尾（含未提交半行） */
    /* 尚未交付的行首 ^D 位置（发生时的 edit_pos），按发生顺序；交付规则见 tty_read() */
    uint32_t  eof_queue[TTY_MAX_PENDING_EOF];
    uint8_t   eof_count;           /* eof_queue 中排队的 EOF 个数 */
    waitq_t   wq_read;            /* 等一整行（或 raw 模式下等至少一个字节）的读者 */
    struct linux_termios tio;     /* 当前 termios 设置 */
    struct winsize       ws;      /* 固定 24x80 */
    /* 前台进程组：^C / ^\ 打给它。初值 1（init 的 pgid），ash 起来之后由 TIOCSPGRP 改写 */
    int16_t   foreground_pgid;
} tty_t;

/* 初始化全局 TTY 单例：自旋锁 + 等待队列 + termios 默认值。
 * 必须在 proc_init() 之前调用（proc_init 会给 init 装 fd 0/1/2，届时 tty 必须已经能用），
 * 且只能由 hart0 调用一次（同 sched_init() 的单例初始化约定）。 */
void tty_init(void);

/* 行规范层：把一个从串口读到的字符喂给 TTY。
 * 中断上下文安全（由 tick_int_handler() 经 tty_poll_input() 调用），全程只用自旋锁，
 * 永不阻塞。 */
void tty_input_push(char c);

/* 轮询 UART 接收寄存器，把读到的字符逐个喂给 tty_input_push()。中断上下文安全；
 * 由 tick_int_handler() 在释放 tick_lock 之后调用，只在 hart0 上执行。 */
void tty_poll_input(void);

/* 设置前台进程组（^C / ^反斜杠 打给它）。由 run_user_program 在第一个用户进程建好时
 * 调用一次，之后由 ioctl(TIOCSPGRP) 接管。 */
void tty_set_foreground_pgid(int16_t pgid);

/* TTY 读端：阻塞直到有一整行可读（raw 模式下至少一个字节）。
 * file->f_private 指向全局 tty 单例。 */
ssize_t tty_read(file_t *file, void *buf, size_t len);

/* TTY 写端：逐字节输出，按 OPOST|ONLCR 做 \n -> \r\n。持 ConsoleLock（不持 tty->lock），
 * 保证整段用户输出不被 printf 打断。 */
ssize_t tty_write(file_t *file, const void *buf, size_t len);

/* 造一个 TTY 设备 file（stdin/stdout/stderr 的后端），f_private 指向全局 tty 单例。
 * f_count 初始为 1，装入多个 fd 时由调用方按需 ++。分配失败返回 NULL。 */
file_t *tty_open_file(void);

/* TTY 的 file_operations_t 表，devfs.c 的 /dev/console、/dev/tty 两个 inode
 * 的 i_fop 直接指向它——不要另建一张内容相同的表，tty_from_file() 靠
 * f_op == &tty_fops 的指针比对判断"是不是真正的 TTY"，两条打开路径
 * （tty_open_file() 直接构造，或走 devfs 挂载后的 vfs_open()）必须共享
 * 同一张表，指针比对才能同时覆盖两条路径。 */
extern file_operations_t tty_fops;

/* 若 f 确实是 tty_fops 打开的 file，返回其对应的 tty_t*；否则（fd 无效、
 * 或不是设备类 file、或是 /dev/null 之类的其它字符设备）返回 NULL。
 * sys_ioctl 用它判断"这是不是一个真正的终端"——不能只看 f_kind==FILE_KIND_DEVICE，
 * 那样会把其它设备的 f_private 错当 tty_t* 解释，是内存安全问题，不只是逻辑错误。 */
tty_t *tty_from_file(file_t *f);

#endif /* _TTY_H_ */
