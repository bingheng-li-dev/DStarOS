/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/*
 * sbi.c —— SBI 固件身份探测
 *
 * 开机打一行"当前固件是谁、什么版本、哪些扩展在"：有些 SBI 扩展在特定固件下是哑的
 * （RustSBI 0.4.0 的 legacy SHUTDOWN / SET_TIMER / send_ipi 调了不报错也不生效），
 * 有这一行，这类问题就是看一眼日志；上板换固件时也靠它确认换成功。
 */

#include "sbi.h"
#include "console.h"

/* sbi.h 只做 extern 声明，唯一的定义在这里。sbi_init() 探测成功前它保持默认值，
 * 那个值代表"固件只实现了 v0.1 legacy"。 */
unsigned long sbi_spec_version = SBI_SPEC_VERSION_DEFAULT;

static long sbi_impl_id = -1;
static long sbi_impl_version = -1;

/**
 * @brief 查询固件是否实现了某个 SBI 扩展
 * @param[in] ext 扩展 ID（SBI_EXT_TIME / SBI_EXT_HSM ...）
 * @retval 0   不支持
 * @retval !=0 支持
 * @note 走 BASE 扩展的 PROBE_EXT。BASE 本身是 SBI v0.2 起的强制扩展，
 *   真碰上只实现 v0.1 的固件时这个调用会返回错误，此时按"不支持"处理。
 */
long sbi_probe_extension(int ext)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_PROBE_EXT,
                                  ext, 0, 0, 0, 0, 0);
    if (ret.error != SBI_SUCCESS)
    {
        return 0;
    }
    return ret.value;
}

/* SBI 规范附录里登记的实现 ID。只列常见的，够认出"是不是我以为的那个固件"即可。 */
static const char *sbi_impl_name(long id)
{
    switch (id)
    {
    case 0:  return "BBL";
    case 1:  return "OpenSBI";
    case 2:  return "Xvisor";
    case 3:  return "KVM";
    case 4:  return "RustSBI";
    case 5:  return "Diosix";
    case 6:  return "Coffer";
    case 7:  return "Xen";
    case 8:  return "PolarFire-HSS";
    default: return "unknown";
    }
}

/**
 * @brief 探测并打印当前 SBI 固件的身份、版本与扩展集
 * @details 必须在 console_init() 之后调用（它要 printf）。
 *   探不到 BASE 扩展（GET_SPEC_VERSION 报错）说明固件只实现了 v0.1 legacy，
 *   此时保留 sbi_spec_version 的默认值并明确打出来——那意味着标准 TIME/IPI/HSM/SRST
 *   一个都不能用，是个必须当场看见的事实，不能静默略过。
 */
void sbi_init(void)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_SPEC_VERSION,
                                  0, 0, 0, 0, 0, 0);
    if (ret.error != SBI_SUCCESS)
    {
        printf("sbi: BASE extension absent, assuming v0.1 legacy-only firmware\n");
        return;
    }
    sbi_spec_version = (unsigned long)ret.value;

    ret = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_IMP_ID, 0, 0, 0, 0, 0, 0);
    sbi_impl_id = ret.error ? -1 : ret.value;
    ret = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_GET_IMP_VERSION, 0, 0, 0, 0, 0, 0);
    sbi_impl_version = ret.error ? -1 : ret.value;

    printf("sbi: spec v%ld.%ld, impl %s (id=%ld, version=0x%lx)\n",
           sbi_major_version(), sbi_minor_version(),
           sbi_impl_name(sbi_impl_id), sbi_impl_id, (unsigned long)sbi_impl_version);

    printf("sbi: ext TIME=%d IPI=%d RFENCE=%d HSM=%d SRST=%d\n",
           sbi_probe_extension(SBI_EXT_TIME)   ? 1 : 0,
           sbi_probe_extension(SBI_EXT_IPI)    ? 1 : 0,
           sbi_probe_extension(SBI_EXT_RFENCE) ? 1 : 0,
           sbi_probe_extension(SBI_EXT_HSM)    ? 1 : 0,
           sbi_probe_extension(SBI_EXT_SRST)   ? 1 : 0);
}
