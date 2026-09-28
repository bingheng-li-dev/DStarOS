/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "console.h"
#include "sync.h"
#include "tinyprintf.h"
#include "sbi.h"
#include "tty.h"
#include "uart.h"
#include "memtype.h"
#include "periph_layout.h"

osslock_t ConsoleLock;
volatile bool panicked = false;

/* 内核日志的换行在这里补成回车+换行（同 Linux uart_console_write 的约定）；
 * 用户输出的换行转换归 tty 的 ONLCR，不能挪进 uart_putc，否则会补两次。 */
static void stdout_putc(void *unused, char ch)
{
    if (ch == '\n')
    {
        uart_putc('\r');
    }
    uart_putc(ch);
}

/* panic 的逃生通道：不经过自有 UART 驱动，直接交给固件，驱动本身出问题时它仍能说话。 */
static void panic_putc(void *unused, char ch)
{
    sbi_console_putchar((int)ch);
}

/**
 * @brief 初始化控制台：UART 与 printf 的输出函数
 */
void console_init(void)
{
    spinlock_init(&ConsoleLock);
    uart_init(mmu_is_enabled() ? pa_to_kva((phyAddr_t)UART) : (virAddr_t)UART);
    init_printf(0, stdout_putc);
}

/**
 * @brief 内核格式化输出，持 ConsoleLock 保证整条不被打断
 */
void printf(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    irq_key_t ConsoleLock_key = spinlock_acquire(&ConsoleLock);
    tfp_format(NULL, stdout_putc, fmt, args);
    spinlock_release(&ConsoleLock, ConsoleLock_key);
    va_end(args);
}

/**
 * @brief 打印 panic 位置与消息后停机；绕开 ConsoleLock，直接走 SBI 输出
 */
void panic_impl(const char *func, int line, char *s, ...)
{
    va_list args;
    /* 绕开 ConsoleLock：panic 可能在持锁上下文里被调用 */
    const char *pre = "\npanic at ";
    for (const char *p = pre; *p; p++) 
    {
        sbi_console_putchar((int)*p);
    }
    for (const char *p = func; *p; p++) 
    {
        sbi_console_putchar((int)*p);
    }
    sbi_console_putchar(':');
    char linebuf[12];
    int i = 0;
    int n = line;
    if (n == 0) 
    {
        linebuf[i++] = '0';
    } 
    else 
    {
        while (n > 0) {
            linebuf[i++] = '0' + (n % 10);
            n /= 10;
        }
        for (int l = 0, r = i - 1; l < r; l++, r--) {
            char tmp = linebuf[l]; linebuf[l] = linebuf[r]; linebuf[r] = tmp;
        }
    }
    for (int j = 0; j < i; j++) 
    {
        sbi_console_putchar((int)linebuf[j]);
    }
    const char *sep = ": ";
    for (const char *p = sep; *p; p++) 
    {
        sbi_console_putchar((int)*p);
    }
    va_start(args, s);
    tfp_format(NULL, panic_putc, s, args);
    va_end(args);
    sbi_console_putchar('\n');
    panicked = true;
    while (true)
        ;
}

/* ============================================================
 * console 设备 file（stdin/stdout/stderr 的后端）
 * ============================================================ */

/* @deprecated 转调 tty_open_file()；调用点换掉后连同 console.h 的声明一起删 */
file_t *console_open_file(void)
{
    return tty_open_file();
}
