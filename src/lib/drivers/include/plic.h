#ifndef __PLIC_H
#define __PLIC_H 

#include <stdint.h>
#include <stddef.h>

/* PLIC 外部中断号。QEMU virt 的取值来自 qemu hw/riscv/virt.c；
 * VF2(JH7110) 的中断号待上板核对，届时按 PLATFORM 分支补。
 * 注：PLIC 目前尚未真正启用（init.c 里 plicInit() 仍是注释），
 * TTY 输入走的是 tick 轮询，见 tty_poll_input()。 */
#define UART_IRQ    10
#define DISK_IRQ    1

/* Enable PLIC for each hart. */
void plicInit(void);
/* Ask PLIC what interrupt we should serve. */
int plicClaim(void);
/* Tell PLIC that we've served this IRQ. */
void plicComplete(int irq);

#endif 
