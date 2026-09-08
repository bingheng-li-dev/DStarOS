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
