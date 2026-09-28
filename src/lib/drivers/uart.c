/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "uart.h"
#include "periph_layout.h"
#include "debug.h"

/* 只要远大于"一整个发送 FIFO 排空"的耗时即可 */
#define UART_TX_SPIN_LIMIT 1000000U

static virAddr_t uart_base;

/* 访问宽度与寄存器间隔按平台区分：JH7110 的 dw-apb-uart 要求 32 位访问（reg-io-width 4）；
 * QEMU virt 的 16550 是字节宽寄存器，整个 MMIO 区只有 8 字节，按 32 位读 base+5 会跨界。 */
#if defined(VF2)
static inline uint32_t uart_read(uint32_t reg)
{
    return *(volatile uint32_t *)(uart_base + UART_REG_OFF(reg));
}

static inline void uart_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(uart_base + UART_REG_OFF(reg)) = val;
}
#else
static inline uint32_t uart_read(uint32_t reg)
{
    return *(volatile uint8_t *)(uart_base + UART_REG_OFF(reg));
}

static inline void uart_write(uint32_t reg, uint32_t val)
{
    *(volatile uint8_t *)(uart_base + UART_REG_OFF(reg)) = (uint8_t)val;
}
#endif

/**
 * @brief 设置 UART 寄存器基址
 * @param[in] base 寄存器块起始地址：MMU 开启前传物理地址，开启后传 pa_to_kva() 换算的高 VA
 * @details 不写任何寄存器，完整继承固件 / U-Boot 留下的配置：
 *   1. 不重配波特率：配错就是满屏乱码，而那时串口是唯一的观察手段；
 *   2. 不写 FCR：FIFO 使能位一变，收发两个 FIFO 会被一并清空，而进内核时发送 FIFO 里
 *      还有固件没发完的字节，清掉就截断开机日志。
 * @note 由 console_init() 调用，MMU 开启前后各一次。基址设置之前调用收发函数会访问 0 地址。
 */
void uart_init(virAddr_t base)
{
    uart_base = base;
}

/**
 * @brief 发送一个字节（轮询）
 * @param[in] c 要发送的字节，原样发出
 * @details 等 LSR.THRE 置位再写 THR。等待有上界：发送器若卡死，宁可丢字，也不要让
 *   持着 ConsoleLock、关着中断的调用者永远挂住（Linux 8250 的 wait_for_xmitr 同样有上界）。
 * @note 不做换行转换：内核日志由 console.c 补回车，用户输出由 tty 的 ONLCR 负责，
 *   放在这里会补两次。不持锁，是否需要 ConsoleLock 由调用者决定。
 */
void uart_putc(char c)
{
    uint32_t n;
    for (n = 0; n < UART_TX_SPIN_LIMIT; n++)
    {
        if ((uart_read(UART_LSR) & UART_LSR_THRE) != 0)
        {
            break;
        }
    }
#if DEBUG_BOOT_TRACE
    static int tx_stuck_reports;
    if (n == UART_TX_SPIN_LIMIT && tx_stuck_reports < 3)
    {
        tx_stuck_reports++;
        BOOT_TRACE("uart_putc: THRE wait hit limit");
    }
#endif
    uart_write(UART_THR, (uint8_t)c);
}

/**
 * @brief 打开 UART 的接收中断（IER.ERBFI）
 * @details 驱动里唯一一次写配置寄存器，先读后写，只动接收位。
 *   发送仍是轮询、不开 THRE 中断：那需要发送缓冲，而控制台输出必须在关中断的
 *   上下文里（panic、持锁路径）也能工作。
 * @note 必须在 PLIC 已把 UART 中断路由到 cpu0、且 tty 已初始化之后调用——
 *   中断一来就会经 tty_poll_input() 把字符推进 tty。
 */
void uart_enable_rx_irq(void)
{
    uart_write(UART_IER, uart_read(UART_IER) | UART_IER_ERBFI);
}

/**
 * @brief UART 中断的控制器侧确认，必须在读接收 FIFO 之前调用
 * @details DesignWare APB UART 有一个 16550 没有的中断源：busy detect（IIR 低 4 位 = 0x7）。
 *   UART 忙时写 LCR 会把它置位，不受 IER 屏蔽，读 LSR / RBR 清不掉，只有读 USR 才清除。
 *   U-Boot 初始化串口若恰好碰上正在发送，进内核时这个中断就已经挂着——PLIC 一使能即刻投递，
 *   complete 之后又立刻重新挂起，主流程被饿死。做法同 Linux 8250_dw 的 dw8250_handle_irq()。
 * @note QEMU 的 16550 没有 busy detect，也没有 USR，这里什么都不做。
 */
void uart_handle_irq(void)
{
#if defined(VF2)
    uint32_t iir = uart_read(UART_IIR);
    bool busy = (iir & UART_IIR_ID_MASK) == UART_IIR_BUSY;
    uint32_t usr = busy ? uart_read(UART_DW_USR) : 0;
#if DEBUG_BOOT_TRACE
    static int irq_reports;
    if (irq_reports < 4)
    {
        irq_reports++;
        boot_trace_hex("uart iir", iir);
        boot_trace_hex("uart usr (read only if busy)", usr);
        boot_trace_hex("uart lsr", uart_read(UART_LSR));
        boot_trace_hex("uart ier", uart_read(UART_IER));
    }
#endif
    (void)usr;
#endif
}

/**
 * @brief 非阻塞地取一个接收到的字节
 * @retval -1 接收缓冲为空
 * @return 其余情况返回 0~255 的字节值
 * @note 中断上下文安全：只读两个寄存器，不持锁、不阻塞。
 */
int uart_getc(void)
{
    if ((uart_read(UART_LSR) & UART_LSR_DR) == 0)
    {
        return -1;
    }
    return (int)(uart_read(UART_RBR) & 0xff);
}
