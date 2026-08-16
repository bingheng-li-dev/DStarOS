#include "sbi.h"
#include "io.h"
#include "plic.h"
#include "console.h"
#include "periph_layout.h"
#include "sync.h"

/* the riscv Platform Level Interrupt Controller (PLIC). */

void plicInit(void)
{
    writel(1, PLIC_V + DISK_IRQ * sizeof(uint32_t));
    writel(1, PLIC_V + UART_IRQ * sizeof(uint32_t));

    uint64_t hart = cpu_get_core_id();
#ifdef QEMU
    // set uart's enable bit for this hart's S-mode.
    *(uint32_t *)PLIC_SENABLE(hart) = (1 << UART_IRQ) | (1 << DISK_IRQ);
    // set this hart's S-mode priority threshold to 0.
    *(uint32_t *)PLIC_SPRIORITY(hart) = 0;
#else
    uint32_t *hart_m_enable = (uint32_t *)PLIC_MENABLE(hart);
    *(hart_m_enable) = readd(hart_m_enable) | (1 << DISK_IRQ);
    uint32_t *hart0_m_int_enable_hi = hart_m_enable + 1;
    *(hart0_m_int_enable_hi) = readd(hart0_m_int_enable_hi) | (1 << (UART_IRQ % 32));
#endif
#ifdef DEBUG
    printf("plicinithart\n");
#endif
}

/* ask the PLIC what interrupt we should serve. */
int plicClaim(void)
{
    uint64_t hart = cpu_get_core_id();
    int irq;
#ifndef QEMU
    irq = *(uint32_t *)PLIC_MCLAIM(hart);
#else
    irq = *(uint32_t *)PLIC_SCLAIM(hart);
#endif
    return irq;
}

/* tell the PLIC we've served this IRQ. */
void plicComplete(int irq)
{
    uint64_t hart = cpu_get_core_id();
#ifndef QEMU
    *(uint32_t *)PLIC_MCLAIM(hart) = irq;
#else
    *(uint32_t *)PLIC_SCLAIM(hart) = irq;
#endif
}
