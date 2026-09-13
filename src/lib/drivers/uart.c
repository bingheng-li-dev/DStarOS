#include "uart.h"
#include "periph_layout.h"

/* 只要远大于"一整个发送 FIFO 排空"的耗时即可 */
#define UART_TX_SPIN_LIMIT 1000000U

static virAddr_t uart_base;

/* 访问宽度与寄存器间隔一样按平台区分：JH7110 的 dw-apb-uart 要求 32 位访问
 * （reg-io-width 4）；QEMU virt 的 16550 是字节宽寄存器，且整个 MMIO 区只有 8 字节，
 * 按 32 位读 base+5 就是一发跨界访问（实测是访问异常）。 */
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
 * @details **不写任何寄存器**，完整继承固件 / U-Boot 留下的配置：
 *   1. 不重配波特率——配错的表现是满屏乱码，而那时串口是唯一的观察手段；
 *      JH7110 上 24 MHz 配 115200 的分频本来就不是整数，重算也不会更准；
 *   2. 不写 FCR——FIFO 使能位一旦变化，收发两个 FIFO 会被一并清空。
 *      VF2 实测进内核时发送 FIFO 里还有固件没发完的字节（USR=0x03），清掉就截断开机日志。
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
    for (uint32_t n = 0; n < UART_TX_SPIN_LIMIT; n++)
    {
        if ((uart_read(UART_LSR) & UART_LSR_THRE) != 0)
        {
            break;
        }
    }
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
