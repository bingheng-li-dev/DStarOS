/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _STARTUP_H_
#define _STARTUP_H_

#include <stdint.h>

/* startup.S 导出的符号。MMU 开启前取到的是物理地址，开启后是高 VA */
extern char _start[];
extern char _trampoline_start[];
extern char _trampoline_end[];
extern char _start_virtual[];

/* 引导核的原始 hartid（未经映射） */
extern uint64_t boot_hartid_raw;
/* 每核引导栈数组，按逻辑 cpu 号分格 */
extern char boot_stacks[];

uint64_t cpu_get_core_id_asm(void);
void cpu_set_core_id_asm(uint64_t core_id);

#endif /* _STARTUP_H_ */
