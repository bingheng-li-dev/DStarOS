/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "vmm.h"
#include "pmm.h"
#include "console.h"
#include "periph_layout.h"
#include "probes.h"

#if DEBUG_MMIO_PROBE
/**
 * @brief 读若干已知的设备寄存器，验证 MMIO 映射确实建立起来了
 * @details 证据互相独立，坏在哪一层就停在哪一组：
 *   1. 页表项：设备 VA 的第 1 级 PTE，确认是 2 MB 叶 PTE、PPN 正确、无 X；
 *   2. 通路判据：QEMU 读 CLINT mtime（两次必不同）；VF2 读 SD 控制器 VERID（高 16 位固定
 *      0x5342），顺带读出 U-Boot 留下的时钟/传输模式状态；
 *   3. UART 身份（仅 VF2）：CTR(+0xfc) 是 DesignWare 固定值 0x44570110；
 *   4. LSR 只打印不判定——真串口发送期间 THRE/TEMT 都是 0，只有 QEMU 的虚拟串口才恒为 1。
 * @note 只读，且避开有副作用的寄存器：不读 UART +0x00（RBR，会弹接收 FIFO）、
 *   +0x08（IIR，会清中断标识）。必须在 trap_init() 之后调用：映射不对时这里是一发
 *   内核缺页或访问异常，stvec 没装好就只剩静默。
 */
void vmm_probe_mmio(void)
{
    virAddr_t base = pa_to_kva((phyAddr_t)UART);

    pte_t *pmd_pte = NULL;
    pte_t *pgd = (pte_t *)pa_to_kva(convert_ppn2pa(vmm_kernel_pgd_ppn));
    if (pte_is_valid(pgd[PGD(base)]))
    {
        pte_t *pmd = (pte_t *)pa_to_kva(convert_ppn2pa(pgd[PGD(base)] >> PTE_PPN_OFFSET));
        pmd_pte = &pmd[PMD(base)];
    }
    printf("mmio: va 0x%lx pgd[%ld]=0x%lx pmd[%ld]=0x%lx\n",
           (unsigned long)base, (long)PGD(base), (unsigned long)pgd[PGD(base)],
           (long)PMD(base), pmd_pte ? (unsigned long)*pmd_pte : 0UL);

#if defined(VF2)
    /* 板上不读 CLINT：OpenSBI 用 PMP 把它划成 M 态独占，S 态一读就是访问异常。
     * 判断能不能读的规则：U-Boot proper 跑在 S 态，它用过的外设 S 态必然可访问——
     * sdio1（fatload mmc）在名单里，CLINT 不在。VERID 高 16 位是固定的 0x5342；
     * CLKENA/CLKDIV 顺带看 U-Boot 把卡时钟留在什么状态。 */
    virAddr_t mmc = pa_to_kva((phyAddr_t)SDMMC_PHYS_BASE);
    printf("mmio: sdmmc verid(+6c)=0x%08x usrid(+68)=0x%08x hcon(+70)=0x%08x (verid should be 0x5342xxxx)\n",
           *(volatile uint32_t *)(mmc + 0x6c), *(volatile uint32_t *)(mmc + 0x68),
           *(volatile uint32_t *)(mmc + 0x70));
    printf("mmio: sdmmc ctrl=0x%08x clkdiv=0x%08x clkena=0x%08x ctype=0x%08x status=0x%08x fifoth=0x%08x\n",
           *(volatile uint32_t *)(mmc + 0x00), *(volatile uint32_t *)(mmc + 0x08),
           *(volatile uint32_t *)(mmc + 0x10), *(volatile uint32_t *)(mmc + 0x18),
           *(volatile uint32_t *)(mmc + 0x48), *(volatile uint32_t *)(mmc + 0x4c));
#else
    volatile uint64_t *mtime = (volatile uint64_t *)(pa_to_kva((phyAddr_t)CLINT) + 0xbff8);
    uint64_t t1 = *mtime;
    uint64_t t2 = *mtime;
    printf("mmio: clint mtime 0x%lx -> 0x%lx (%s)\n", (unsigned long)t1, (unsigned long)t2,
           (t2 != t1) ? "ticking, MMIO path OK" : "STUCK");

#endif
    /* 这一批只在 VF2 上读：QEMU virt 的 16550 MMIO 区只有 8 字节（serial_mm_init 按
     * regshift 0 注册），读 +0x0c 及以后落在未分配物理地址上，拿到的是访问异常而非缺页。 */
#if defined(VF2)
    printf("mmio: uart w32 +04=0x%08x +0c=0x%08x +14=0x%08x +18=0x%08x +7c=0x%08x\n",
           *(volatile uint32_t *)(base + 0x04), *(volatile uint32_t *)(base + 0x0c),
           *(volatile uint32_t *)(base + 0x14), *(volatile uint32_t *)(base + 0x18),
           *(volatile uint32_t *)(base + 0x7c));

    printf("mmio: uart ucv(+f8)=0x%08x ctr(+fc)=0x%08x (ctr should be 0x44570110)\n",
           *(volatile uint32_t *)(base + 0xf8), *(volatile uint32_t *)(base + 0xfc));

    printf("mmio: uart b8 +05=0x%02x +14=0x%02x\n",
           *(volatile uint8_t *)(base + 0x05), *(volatile uint8_t *)(base + 0x14));
#endif

    /* 访问宽度也要分平台，不只是偏移：QEMU 那颗是字节宽寄存器，按 32 位读 base+5
     * 就是一发跨界的非对齐访问。 */
#if defined(VF2)
    uint32_t lsr = *(volatile uint32_t *)(base + UART_REG_OFF(UART_LSR));
#else
    uint32_t lsr = *(volatile uint8_t *)(base + UART_REG_OFF(UART_LSR));
#endif
    printf("mmio: uart lsr = 0x%02x (THRE=%d, DR=%d)\n", lsr,
           (lsr & UART_LSR_THRE) ? 1 : 0, (lsr & UART_LSR_DR) ? 1 : 0);
}
#endif
