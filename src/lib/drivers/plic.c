#include "plic.h"
#include "periph_layout.h"
#include "memtype.h"
#include "console.h"
#include "cpu.h"

static inline uint32_t plic_read(uint64_t off)
{
    return *(volatile uint32_t *)(pa_to_kva((phyAddr_t)PLIC) + off);
}

static inline void plic_write(uint64_t off, uint32_t val)
{
    *(volatile uint32_t *)(pa_to_kva((phyAddr_t)PLIC) + off) = val;
}

static uint64_t plic_current_s_context(void)
{
    return PLIC_S_CONTEXT(cpu_get_hartid((int)cpu_get_core_id()));
}

/**
 * @brief 配置 PLIC：把 UART 接收中断只路由给 cpu0
 * @details
 *   1. 其余在位 cpu 的 S context 清掉 UART 的使能位——进内核时 enable 寄存器的
 *      状态取决于固件，不做假设；
 *   2. UART 中断优先级置 1（必须大于阈值才会投递）；
 *   3. cpu0 的 S context 阈值置 0、置上 UART 的使能位。
 *
 *   **只给 cpu0**：tty_poll_input() 只在 cpu0 上真正读接收 FIFO，别的核 claim 到之后
 *   直接返回，FIFO 没被取空，电平触发的 UART 中断会立刻重新挂起。
 * @note 只在 cpu0 上调用一次，必须排在 tty_init() 之后、uart_enable_rx_irq() 之前。
 *   只触碰在位 cpu 的 S context——VF2 上 hart 0（S7）的 M context 一个字节都不写。
 */
void plic_init(void)
{
    for (int cpu = 1; cpu < cpu_get_present_count(); cpu++)
    {
        uint64_t ctx = PLIC_S_CONTEXT(cpu_get_hartid(cpu));
        plic_write(PLIC_ENABLE_OFF(ctx, UART_IRQ),
                   plic_read(PLIC_ENABLE_OFF(ctx, UART_IRQ)) & ~PLIC_ENABLE_BIT(UART_IRQ));
    }

    plic_write(PLIC_PRIORITY_OFF(UART_IRQ), 1);

    uint64_t ctx0 = PLIC_S_CONTEXT(cpu_get_hartid(0));
    plic_write(PLIC_THRESHOLD_OFF(ctx0), 0);
    plic_write(PLIC_ENABLE_OFF(ctx0, UART_IRQ),
               plic_read(PLIC_ENABLE_OFF(ctx0, UART_IRQ)) | PLIC_ENABLE_BIT(UART_IRQ));

    printf("plic: uart irq %d -> cpu0 (hart %ld, s-context %ld)\n",
           UART_IRQ, (long)cpu_get_hartid(0), (long)ctx0);
}

/**
 * @brief 领取当前 hart 上待处理的最高优先级外部中断
 * @retval 0 没有待处理的中断（电平已撤销，或被别的 context 领走）
 * @return 其余情况返回中断号
 * @note 读 claim 寄存器本身就是"领取"动作。领到非 0 的中断必须用 plic_complete() 交还，
 *   否则同一中断源不会再次投递。
 */
uint32_t plic_claim(void)
{
    return plic_read(PLIC_CLAIM_OFF(plic_current_s_context()));
}

/**
 * @brief 告知 PLIC 当前 hart 已处理完某个中断
 * @param[in] irq plic_claim() 领到的中断号
 */
void plic_complete(uint32_t irq)
{
    plic_write(PLIC_CLAIM_OFF(plic_current_s_context()), irq);
}
