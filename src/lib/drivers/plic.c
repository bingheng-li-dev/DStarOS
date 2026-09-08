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
    // set uart's enable bit for this hart's S-mode.
    *(uint32_t *)PLIC_SENABLE(hart) = (1 << UART_IRQ) | (1 << DISK_IRQ);
    // set this hart's S-mode priority threshold to 0.
    *(uint32_t *)PLIC_SPRIORITY(hart) = 0;
#ifdef DEBUG
    printf("plicinithart\n");
#endif
}

/* ask the PLIC what interrupt we should serve. */
int plicClaim(void)
{
    uint64_t hart = cpu_get_core_id();
    int irq = *(uint32_t *)PLIC_SCLAIM(hart);
    return irq;
}

/* tell the PLIC we've served this IRQ. */
void plicComplete(int irq)
{
    uint64_t hart = cpu_get_core_id();
    *(uint32_t *)PLIC_SCLAIM(hart) = irq;
}
