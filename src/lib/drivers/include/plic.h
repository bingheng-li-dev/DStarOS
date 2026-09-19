/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef __PLIC_H
#define __PLIC_H

#include <stdint.h>

/* UART0 的 PLIC 中断号。QEMU virt 取自 qemu hw/riscv/virt.c；
 * VF2 实测自 /soc/serial@10000000 的 interrupts = <0x20>。 */
#if defined(VF2)
#define UART_IRQ    32
#else
#define UART_IRQ    10
#endif

void     plic_init(void);
uint32_t plic_claim(void);
void     plic_complete(uint32_t irq);

#endif
