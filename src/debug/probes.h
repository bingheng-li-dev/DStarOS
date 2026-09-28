/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _PROBES_H_
#define _PROBES_H_

#include "debug.h"

#if DEBUG_MMIO_PROBE
void vmm_probe_mmio(void);
#endif

#if DEBUG_BRINGUP
void vmm_dump_boot_mappings(void);
#endif

#if DEBUG_PTE_AD_PROBE
void vmm_probe_pte_ad(void);
#endif

#if defined(VF2) && DEBUG_SDMMC_PROBE
void sdmmc_fs_probe(void);
#endif

#endif /* _PROBES_H_ */
