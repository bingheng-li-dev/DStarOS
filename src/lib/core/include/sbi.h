/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Trimmed from Linux arch/riscv/include/asm/sbi.h for DStarOS.
 * Keeps only extensions needed for the BusyBox target path:
 * v0.1 legacy, BASE, TIME, IPI, RFENCE, HSM, SRST.
 *
 * SMP note: DStarOS has 2 harts (hart 0 / hart 1 on K210).
 * hart_mask is a plain unsigned long bitmask (bit N = hart N).
 * No cpumask_t needed.
 *
 * All functions are static inline for now; version-aware dispatch
 * (TIME/IPI/RFENCE new extensions) will move to sbi.c when needed.
 */

#ifndef _SBI_H_
#define _SBI_H_

#include <stdint.h>
#include <stdbool.h>

#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

/* ------------------------------------------------------------------ */
/* Extension IDs                                                        */
/* ------------------------------------------------------------------ */

enum sbi_ext_id {
    /* Legacy v0.1 — RustSBI 0.4 maintains backward compat */
    SBI_EXT_0_1_SET_TIMER                = 0x0,
    SBI_EXT_0_1_CONSOLE_PUTCHAR          = 0x1,
    SBI_EXT_0_1_CONSOLE_GETCHAR          = 0x2,
    SBI_EXT_0_1_CLEAR_IPI                = 0x3,
    SBI_EXT_0_1_SEND_IPI                 = 0x4,
    SBI_EXT_0_1_REMOTE_FENCE_I           = 0x5,
    SBI_EXT_0_1_REMOTE_SFENCE_VMA        = 0x6,
    SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID   = 0x7,
    SBI_EXT_0_1_SHUTDOWN                 = 0x8,

    /* Standard extensions (SBI v0.2+, supported by RustSBI 0.4) */
    SBI_EXT_BASE   = 0x10,
    SBI_EXT_TIME   = 0x54494D45,   /* "TIME" */
    SBI_EXT_IPI    = 0x735049,     /* "sPI"  — cross-hart IPI */
    SBI_EXT_RFENCE = 0x52464E43,   /* "RFNC" — remote TLB shootdown */
    SBI_EXT_HSM    = 0x48534D,     /* "HSM"  — hart start/stop */
    SBI_EXT_SRST   = 0x53525354,   /* "SRST" — system reset */
};

/* ------------------------------------------------------------------ */
/* Function IDs                                                         */
/* ------------------------------------------------------------------ */

enum sbi_ext_base_fid {
    SBI_EXT_BASE_GET_SPEC_VERSION = 0,
    SBI_EXT_BASE_GET_IMP_ID,
    SBI_EXT_BASE_GET_IMP_VERSION,
    SBI_EXT_BASE_PROBE_EXT,
    SBI_EXT_BASE_GET_MVENDORID,
    SBI_EXT_BASE_GET_MARCHID,
    SBI_EXT_BASE_GET_MIMPID,
};

enum sbi_ext_time_fid {
    SBI_EXT_TIME_SET_TIMER = 0,
};

enum sbi_ext_ipi_fid {
    SBI_EXT_IPI_SEND_IPI = 0,
};

enum sbi_ext_rfence_fid {
    SBI_EXT_RFENCE_REMOTE_FENCE_I = 0,
    SBI_EXT_RFENCE_REMOTE_SFENCE_VMA,
    SBI_EXT_RFENCE_REMOTE_SFENCE_VMA_ASID,
};

enum sbi_ext_hsm_fid {
    SBI_EXT_HSM_HART_START = 0,
    SBI_EXT_HSM_HART_STOP,
    SBI_EXT_HSM_HART_STATUS,
    SBI_EXT_HSM_HART_SUSPEND,
};

enum sbi_hsm_hart_state {
    SBI_HSM_STATE_STARTED = 0,
    SBI_HSM_STATE_STOPPED,
    SBI_HSM_STATE_START_PENDING,
    SBI_HSM_STATE_STOP_PENDING,
    SBI_HSM_STATE_SUSPENDED,
    SBI_HSM_STATE_SUSPEND_PENDING,
    SBI_HSM_STATE_RESUME_PENDING,
};

enum sbi_ext_srst_fid {
    SBI_EXT_SRST_RESET = 0,
};

enum sbi_srst_reset_type {
    SBI_SRST_RESET_TYPE_SHUTDOWN    = 0,
    SBI_SRST_RESET_TYPE_COLD_REBOOT,
    SBI_SRST_RESET_TYPE_WARM_REBOOT,
};

enum sbi_srst_reset_reason {
    SBI_SRST_RESET_REASON_NONE         = 0,
    SBI_SRST_RESET_REASON_SYS_FAILURE,
};

/* ------------------------------------------------------------------ */
/* SBI spec version fields                                              */
/* ------------------------------------------------------------------ */

#define SBI_SPEC_VERSION_DEFAULT     0x1
#define SBI_SPEC_VERSION_MAJOR_SHIFT 24
#define SBI_SPEC_VERSION_MAJOR_MASK  0x7f
#define SBI_SPEC_VERSION_MINOR_MASK  0xffffff

/* ------------------------------------------------------------------ */
/* SBI return error codes                                               */
/* ------------------------------------------------------------------ */

#define SBI_SUCCESS              0
#define SBI_ERR_FAILURE         -1
#define SBI_ERR_NOT_SUPPORTED   -2
#define SBI_ERR_INVALID_PARAM   -3
#define SBI_ERR_DENIED          -4
#define SBI_ERR_INVALID_ADDRESS -5
#define SBI_ERR_ALREADY_AVAILABLE -6
#define SBI_ERR_ALREADY_STARTED -7
#define SBI_ERR_ALREADY_STOPPED -8
#define SBI_ERR_INVALID_STATE   -10
#define SBI_ERR_BAD_RANGE       -11
#define SBI_ERR_TIMEOUT         -12

/* ------------------------------------------------------------------ */
/* SBI call mechanism                                                   */
/*                                                                      */
/* New convention (v0.2+): a7=ext, a6=fid, a0-a5=args                  */
/*   returns: a0=error, a1=value  → struct sbiret                      */
/* Legacy (v0.1): a7=ext, no fid register, result in a0 only           */
/* ------------------------------------------------------------------ */

struct sbiret {
    long error;
    long value;
};

static inline struct sbiret __sbi_ecall(unsigned long a0, unsigned long a1,
                                         unsigned long a2, unsigned long a3,
                                         unsigned long a4, unsigned long a5,
                                         int fid, int ext)
{
    struct sbiret ret;
    register unsigned long r_a0 asm("a0") = a0;
    register unsigned long r_a1 asm("a1") = a1;
    register unsigned long r_a2 asm("a2") = a2;
    register unsigned long r_a3 asm("a3") = a3;
    register unsigned long r_a4 asm("a4") = a4;
    register unsigned long r_a5 asm("a5") = a5;
    register unsigned long r_a6 asm("a6") = (unsigned long)fid;
    register unsigned long r_a7 asm("a7") = (unsigned long)ext;
    asm volatile("ecall"
                 : "+r"(r_a0), "+r"(r_a1)
                 : "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5),
                   "r"(r_a6), "r"(r_a7)
                 : "memory");
    ret.error = r_a0;
    ret.value = r_a1;
    return ret;
}

#define sbi_ecall(ext, fid, a0, a1, a2, a3, a4, a5)        \
    __sbi_ecall((unsigned long)(a0), (unsigned long)(a1),   \
                (unsigned long)(a2), (unsigned long)(a3),   \
                (unsigned long)(a4), (unsigned long)(a5),   \
                (fid), (ext))

/*
 * Legacy v0.1 ecall: a7=which, a0-a2=args, result in a0.
 * SBI_CALL_0/1/2 kept as aliases for source compatibility.
 */
#define SBI_CALL_LEGACY(which, arg0, arg1, arg2) ({             \
    register uintptr_t a0 asm("a0") = (uintptr_t)(arg0);       \
    register uintptr_t a1 asm("a1") = (uintptr_t)(arg1);       \
    register uintptr_t a2 asm("a2") = (uintptr_t)(arg2);       \
    register uintptr_t a7 asm("a7") = (uintptr_t)(which);      \
    asm volatile("ecall"                                        \
                 : "+r"(a0)                                     \
                 : "r"(a1), "r"(a2), "r"(a7)                   \
                 : "memory");                                   \
    a0;                                                         \
})
#define SBI_CALL(which, arg0, arg1, arg2) SBI_CALL_LEGACY(which, arg0, arg1, arg2)
#define SBI_CALL_0(which)                 SBI_CALL_LEGACY(which, 0, 0, 0)
#define SBI_CALL_1(which, arg0)           SBI_CALL_LEGACY(which, arg0, 0, 0)
#define SBI_CALL_2(which, arg0, arg1)     SBI_CALL_LEGACY(which, arg0, arg1, 0)

/* ------------------------------------------------------------------ */
/* SBI spec version state + helpers                                     */
/* ------------------------------------------------------------------ */

extern unsigned long sbi_spec_version;

static inline int sbi_spec_is_0_1(void)
{
    return (sbi_spec_version == SBI_SPEC_VERSION_DEFAULT) ? 1 : 0;
}

static inline unsigned long sbi_major_version(void)
{
    return (sbi_spec_version >> SBI_SPEC_VERSION_MAJOR_SHIFT) &
           SBI_SPEC_VERSION_MAJOR_MASK;
}

static inline unsigned long sbi_minor_version(void)
{
    return sbi_spec_version & SBI_SPEC_VERSION_MINOR_MASK;
}

static inline unsigned long sbi_mk_version(unsigned long major,
                                            unsigned long minor)
{
    return ((major & SBI_SPEC_VERSION_MAJOR_MASK) << SBI_SPEC_VERSION_MAJOR_SHIFT) |
           (minor  & SBI_SPEC_VERSION_MINOR_MASK);
}

/* Map SBI error codes to DStarOS ENO* (see errorcode.h). */
static inline int sbi_err_to_errno(int err)
{
    switch (err) {
    case SBI_SUCCESS:             return  0;   /* ENO0_NO_ERROR */
    case SBI_ERR_INVALID_PARAM:
    case SBI_ERR_INVALID_STATE:
    case SBI_ERR_BAD_RANGE:       return -6;   /* ENO6_INVAL_PARAM */
    case SBI_ERR_DENIED:          return -16;  /* ENO16_PERM */
    case SBI_ERR_INVALID_ADDRESS: return -6;   /* ENO6_INVAL_PARAM */
    case SBI_ERR_NOT_SUPPORTED:   return -5;   /* ENO5_NOSUCH_ENTRY */
    case SBI_ERR_TIMEOUT:         return -4;   /* ENO4_BUSY */
    default:                      return -1;   /* ENO1_NOMORE_MEM (generic) */
    }
}

/* ------------------------------------------------------------------ */
/* Console (v0.1 legacy — always available via RustSBI)                 */
/* ------------------------------------------------------------------ */

static inline void sbi_console_putchar(int ch)
{
    SBI_CALL_LEGACY(SBI_EXT_0_1_CONSOLE_PUTCHAR, ch, 0, 0);
}

static inline int sbi_console_getchar(void)
{
    return (int)SBI_CALL_LEGACY(SBI_EXT_0_1_CONSOLE_GETCHAR, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Timer (v0.1 legacy)                                                  */
/* ------------------------------------------------------------------ */

static inline void sbi_set_timer(uint64_t stime_value)
{
#if __riscv_xlen == 32
    SBI_CALL_LEGACY(SBI_EXT_0_1_SET_TIMER, stime_value, stime_value >> 32, 0);
#else
    SBI_CALL_LEGACY(SBI_EXT_0_1_SET_TIMER, stime_value, 0, 0);
#endif
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                             */
/* ------------------------------------------------------------------ */

static inline void sbi_shutdown(void)
{
    SBI_CALL_LEGACY(SBI_EXT_0_1_SHUTDOWN, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* IPI / remote fence                                                   */
/* hart_mask: bitmask of target harts (bit 0=hart 0, bit 1=hart 1).   */
/*                                                                      */
/* Callers must pass the VALUE, not a pointer:                          */
/*   unsigned long mask = BIT(1);  sbi_send_ipi(mask);                 */
/*                                                                      */
/* Note: v0.1 takes a pointer in a0; wrap the local here so callers    */
/* stay pointer-free.  When IPI extension is probed use sbi_ecall.     */
/* ------------------------------------------------------------------ */

static inline void sbi_clear_ipi(void)
{
    SBI_CALL_LEGACY(SBI_EXT_0_1_CLEAR_IPI, 0, 0, 0);
}

static inline void sbi_send_ipi(unsigned long hart_mask)
{
    SBI_CALL_LEGACY(SBI_EXT_0_1_SEND_IPI, &hart_mask, 0, 0);
}

static inline int sbi_remote_fence_i(unsigned long hart_mask)
{
    SBI_CALL_LEGACY(SBI_EXT_0_1_REMOTE_FENCE_I, &hart_mask, 0, 0);
    return 0;
}

static inline int sbi_remote_sfence_vma(unsigned long hart_mask,
                                         unsigned long start,
                                         unsigned long size)
{
    SBI_CALL_LEGACY(SBI_EXT_0_1_REMOTE_SFENCE_VMA, &hart_mask, start, size);
    return 0;
}

static inline int sbi_remote_sfence_vma_asid(unsigned long hart_mask,
                                               unsigned long start,
                                               unsigned long size,
                                               unsigned long asid)
{
    (void)asid;
    SBI_CALL_LEGACY(SBI_EXT_0_1_REMOTE_SFENCE_VMA_ASID, &hart_mask, start, size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hart State Management (HSM)                                          */
/* Can replace the core2Enabled polling mechanism for hart 1 start.    */
/* ------------------------------------------------------------------ */

static inline int sbi_hsm_hart_start(unsigned long hartid,
                                      unsigned long start_addr,
                                      unsigned long priv)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
                                  hartid, start_addr, priv, 0, 0, 0);
    return (int)ret.error;
}

static inline int sbi_hsm_hart_stop(void)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_STOP,
                                  0, 0, 0, 0, 0, 0);
    return (int)ret.error;
}

static inline int sbi_hsm_hart_status(unsigned long hartid)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_STATUS,
                                  hartid, 0, 0, 0, 0, 0);
    if (ret.error)
        return (int)ret.error;
    return (int)ret.value;
}

/* ------------------------------------------------------------------ */
/* Vendor extension: enable M-mode external interrupt forwarding        */
/* Custom call used in trap.c; kept as-is.                              */
/* ------------------------------------------------------------------ */

static inline void sbi_set_mie(void)
{
    SBI_CALL_LEGACY(0x0A000005, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Probe / version (need sbi.c when implemented)                        */
/* ------------------------------------------------------------------ */

void sbi_init(void);
long sbi_probe_extension(int ext);
long sbi_get_mvendorid(void);
long sbi_get_marchid(void);
long sbi_get_mimpid(void);

#endif /* _SBI_H_ */
