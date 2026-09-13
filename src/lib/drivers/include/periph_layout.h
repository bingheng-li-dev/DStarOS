#ifndef _PERIPH_LAYOUT_H_
#define _PERIPH_LAYOUT_H_

/* 外设物理地址布局。
 *
 * QEMU virt（见 qemu hw/riscv/virt.c）：
 *   0x0200_0000  CLINT
 *   0x0C00_0000  PLIC
 *   0x1000_0000  UART0
 *   0x1000_1000  virtio disk
 *   0x8000_0000  RAM（RustSBI 在前，内核从 0x8020_0000 起）
 *
 * ⚠️ **下面的 *_V 宏目前没有任何映射代码去落实它**。`VIRT_OFFSET` 是早期从 xv6
 * 抄来的一套设备虚拟地址方案，与内核实际在用的偏移映射
 * （KVA = PA + KERNEL_VA_OFFSET，见 memtype.h）**不是一套体系**——内核页表只映射了
 * RAM，没有映射任何 MMIO。这也正是 init.c 里 plicInit() 一直被注释掉的原因。
 * 真要用 MMIO 时，应当改成 pa_to_kva(PA) 并在内核页表里补设备段映射，
 * 而不是沿用 VIRT_OFFSET。
 *
 * VF2(JH7110) 的外设地址待上板核对后按 PLATFORM 分支补。
 */

#define VIRT_OFFSET             0x3F00000000L

#define UART                    0x10000000L
#define UART_V                  (UART + VIRT_OFFSET)

/* UART0 的寄存器排布。两个平台的基址相同，但访问方式不同：
 * QEMU virt 的 ns16550a 是 8 位寄存器逐字节排列（reg-shift 0）；
 * JH7110 的 snps,dw-apb-uart 要求 32 位访问、寄存器间隔 4 字节
 * （reg-shift 2 + reg-io-width 4，实测自 /soc/serial@10000000）。
 * 下面的寄存器号是 16550 的**逻辑编号**，取字节偏移要按 UART_REG_SHIFT 左移。 */
#if defined(VF2)
#define UART_REG_SHIFT          2
#else
#define UART_REG_SHIFT          0
#endif
#define UART_REG_OFF(reg)       ((reg) << UART_REG_SHIFT)

#define UART_RBR                0       /* Receiver Buffer Register（读） */
#define UART_THR                0       /* Transmitter Holding Register（写） */
#define UART_LSR                5       /* Line Status Register */
#define UART_LSR_DR             (1 << 0) /* 接收数据就绪 */
#define UART_LSR_THRE           (1 << 5) /* 发送保持寄存器空 */

/* SD 卡槽控制器（snps,dw-mshc）。JH7110 上是 /soc/sdio1@16020000；
 * 注意 sdio0@16010000 是板载 eMMC 接口，不是 SD 槽（U-Boot 里 SD 卡是 mmc 1）。 */
#if defined(VF2)
#define SDMMC_PHYS_BASE         0x16020000UL
#endif

/* virtio mmio interface */
#define VIRTIO0                 0x10001000
#define VIRTIO0_V               (VIRTIO0 + VIRT_OFFSET)

/* local interrupt controller, which contains the timer. */
#define CLINT                   0x02000000L
#define CLINT_V                 (CLINT + VIRT_OFFSET)

#define PLIC                    0x0c000000L
#define PLIC_V                  (PLIC + VIRT_OFFSET)

#define PLIC_PRIORITY           (PLIC_V + 0x0)
#define PLIC_PENDING            (PLIC_V + 0x1000)
#define PLIC_MENABLE(hart)      (PLIC_V + 0x2000 + (hart) * 0x100)
#define PLIC_SENABLE(hart)      (PLIC_V + 0x2080 + (hart) * 0x100)
#define PLIC_MPRIORITY(hart)    (PLIC_V + 0x200000 + (hart) * 0x2000)
#define PLIC_SPRIORITY(hart)    (PLIC_V + 0x201000 + (hart) * 0x2000)
#define PLIC_MCLAIM(hart)       (PLIC_V + 0x200004 + (hart) * 0x2000)
#define PLIC_SCLAIM(hart)       (PLIC_V + 0x201004 + (hart) * 0x2000)

#endif
