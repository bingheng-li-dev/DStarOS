/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _PERIPH_LAYOUT_H_
#define _PERIPH_LAYOUT_H_

/* 外设物理地址布局。两个平台的这几个基址恰好相同（QEMU virt 与 JH7110）：
 *   0x0200_0000  CLINT（VF2 上被 OpenSBI 的 PMP 划成 M 态独占，S 态读是访问异常）
 *   0x0C00_0000  PLIC
 *   0x1000_0000  UART0
 *   0x1602_0000  SD 卡槽控制器（仅 VF2）
 * 访问一律经 pa_to_kva(PA)：设备区由 vmm_map_mmio_range() 以 2 MB 大页映射进内核高半区，
 * 与 RAM 共用同一个偏移，不需要第二套换算。 */

#define UART                    0x10000000L

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
#define UART_IER                1       /* Interrupt Enable Register */
#define UART_IER_ERBFI          (1 << 0) /* 接收数据可用中断 */
#define UART_LSR                5       /* Line Status Register */
#define UART_LSR_DR             (1 << 0) /* 接收数据就绪 */
#define UART_LSR_THRE           (1 << 5) /* 发送保持寄存器空 */
#define UART_IIR                2       /* Interrupt Identification Register（读） */
#define UART_IIR_ID_MASK        0x0f
#define UART_IIR_BUSY           0x07    /* DesignWare 专有：busy detect，只有读 USR 才清得掉 */
#if defined(VF2)
/* DesignWare APB UART 专有的 UART Status Register（偏移 0x7c，即逻辑号 31）。
 * QEMU 的 16550 没有它，而且那边 MMIO 区只有 8 字节，访问即访问异常。 */
#define UART_DW_USR             31
#endif

/* SD 卡槽控制器（snps,dw-mshc）。JH7110 上是 /soc/sdio1@16020000；
 * 注意 sdio0@16010000 是板载 eMMC 接口，不是 SD 槽（U-Boot 里 SD 卡是 mmc 1）。 */
#if defined(VF2)
#define SDMMC_PHYS_BASE         0x16020000UL
#endif

#define CLINT                   0x02000000L

#define PLIC                    0x0c000000L

/* PLIC 寄存器偏移，一律按 context 编号索引（RISC-V PLIC 规范布局）。
 * enable 位图每 32 个中断号占一个字：IRQ 号一跨过 32 就必须按字取，
 * 直接写 1 << irq 在 irq >= 32 时是未定义行为（VF2 的 UART 恰好是 32）。 */
#define PLIC_PRIORITY_OFF(irq)          ((irq) * 4)
#define PLIC_ENABLE_OFF(ctx, irq)       (0x2000 + (ctx) * 0x80 + ((irq) / 32) * 4)
#define PLIC_ENABLE_BIT(irq)            ((uint32_t)1 << ((irq) % 32))
#define PLIC_THRESHOLD_OFF(ctx)         (0x200000 + (ctx) * 0x1000)
#define PLIC_CLAIM_OFF(ctx)             (0x200004 + (ctx) * 0x1000)

/* hart 的 S 态 context 编号。**两个平台不能共用一个公式**：PLIC 按 hart 顺序排 context，
 * 支持 S 态的 hart 占 M、S 两个，而 JH7110 的 hart 0 是没有 S 态的 S7，只占一个 M，
 * 其后整体错位一格。
 *   QEMU virt：每个 hart 都是 <M 11><S 9>          → S context = 2 * hart + 1
 *   VF2：实测 PLIC 节点 interrupts-extended 为
 *        <h0 11><h1 11><h1 9><h2 11><h2 9>...       → S context = 2 * hart（hart >= 1）
 * ⚠️ VF2 上按此算出的 enable / threshold / claim 地址，数值上恰好等于 xv6 那套
 * "hart N 的 M 态"宏——纯属巧合，不是在操作 M 态 context。 */
#if defined(VF2)
#define PLIC_S_CONTEXT(hart)            (2 * (hart))
#else
#define PLIC_S_CONTEXT(hart)            (2 * (hart) + 1)
#endif

#endif
