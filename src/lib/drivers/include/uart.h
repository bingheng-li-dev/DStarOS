#ifndef _UART_H_
#define _UART_H_

#include "memtype.h"

void uart_init(virAddr_t base);
void uart_putc(char c);
int  uart_getc(void);
void uart_enable_rx_irq(void);
void uart_handle_irq(void);

#endif
