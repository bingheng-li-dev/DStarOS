/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/*
 * sbi.h —— RISC-V SBI 调用封装
 *
 * 依据 RISC-V SBI 规范（riscv-non-isa/riscv-sbi-doc）编写，未参照任何具体
 * 实现的源码。涉及章节：Binary Encoding、Base、TIME、IPI、HSM、SRST，以及
 * Legacy 扩展中仍在使用的 Console Putchar。
 *
 * 只声明本内核实际调用到的部分。规范里其余的扩展、功能号、错误码和状态
 * 枚举都没有列进来，要用时照规范补；凡是被裁掉的，下面都在注释里点了名，
 * 以免补的时候漏看隐式递增的枚举值。
 *
 * hart_mask 按规范是一个标量位向量（bit N 对应 hart_mask_base + N），
 * 不需要额外的 cpumask 类型。
 */

#ifndef _SBI_H_
#define _SBI_H_

#include <stdint.h>

/* 与 SBI 无关的通用位掩码工具，cpu.c 组装 hart_mask 时用它。 */
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif

/* ------------------------------------------------------------------ */
/* 扩展 ID（EID）                                                       */
/* ------------------------------------------------------------------ */

enum sbi_ext_id
{
    /* Legacy 扩展占 EID 0x00~0x0F，规范已整体标记为废弃。这里只留控制台输出
     * 一项，理由见 sbi_console_putchar()。 */
    SBI_EXT_0_1_CONSOLE_PUTCHAR = 0x1,

    SBI_EXT_BASE   = 0x10,
    SBI_EXT_TIME   = 0x54494D45,   /* "TIME" */
    SBI_EXT_IPI    = 0x735049,     /* "sPI"  */
    SBI_EXT_RFENCE = 0x52464E43,   /* "RFNC" */
    SBI_EXT_HSM    = 0x48534D,     /* "HSM"  */
    SBI_EXT_SRST   = 0x53525354,   /* "SRST" */
};

/* ------------------------------------------------------------------ */
/* 功能 ID（FID）                                                       */
/* ------------------------------------------------------------------ */

/* 规范中 BASE 还有 GET_MVENDORID(4)、GET_MARCHID(5)、GET_MIMPID(6)。 */
enum sbi_ext_base_fid
{
    SBI_EXT_BASE_GET_SPEC_VERSION = 0,
    SBI_EXT_BASE_GET_IMP_ID       = 1,
    SBI_EXT_BASE_GET_IMP_VERSION  = 2,
    SBI_EXT_BASE_PROBE_EXT        = 3,
};

enum sbi_ext_time_fid
{
    SBI_EXT_TIME_SET_TIMER = 0,
};

enum sbi_ext_ipi_fid
{
    SBI_EXT_IPI_SEND_IPI = 0,
};

/* 规范中 HSM 还有 HART_STOP(1) 与 HART_SUSPEND(3)。
 * HART_STATUS 的值必须显式写成 2：中间的 HART_STOP 没有列出来，靠枚举递增
 * 会得到 1，那是 HART_STOP 的功能号，会静默调错。 */
enum sbi_ext_hsm_fid
{
    SBI_EXT_HSM_HART_START  = 0,
    SBI_EXT_HSM_HART_STATUS = 2,
};

enum sbi_ext_srst_fid
{
    SBI_EXT_SRST_RESET = 0,
};

/* 规范中还有 COLD_REBOOT(1) 与 WARM_REBOOT(2)。 */
enum sbi_srst_reset_type
{
    SBI_SRST_RESET_TYPE_SHUTDOWN = 0,
};

/* 规范中还有 SYS_FAILURE(1)。 */
enum sbi_srst_reset_reason
{
    SBI_SRST_RESET_REASON_NONE = 0,
};

/* ------------------------------------------------------------------ */
/* 规范版本号字段                                                       */
/*                                                                      */
/* 低 24 位是次版本号，接着 7 位是主版本号，bit 31 保留且必须为 0。       */
/* ------------------------------------------------------------------ */

#define SBI_SPEC_VERSION_DEFAULT     0x1
#define SBI_SPEC_VERSION_MAJOR_SHIFT 24
#define SBI_SPEC_VERSION_MAJOR_MASK  0x7f
#define SBI_SPEC_VERSION_MINOR_MASK  0xffffff

/* ------------------------------------------------------------------ */
/* 返回错误码                                                           */
/*                                                                      */
/* 规范共定义 SBI_SUCCESS 与 SBI_ERR_FAILED(-1) 起的十余个错误码，        */
/* 这里只列内核代码里真正比较过的两个。                                  */
/* ------------------------------------------------------------------ */

#define SBI_SUCCESS             0
#define SBI_ERR_INVALID_PARAM (-3)

/* ------------------------------------------------------------------ */
/* 调用约定                                                             */
/*                                                                      */
/* v0.2 起：a7=EID，a6=FID，a0~a5 为参数，返回 a0=error、a1=value。       */
/* Legacy：a7=EID，没有 FID 寄存器，结果只在 a0，a1 不返回任何东西。      */
/* 除 a0、a1 外的寄存器由被调用方负责保持。                              */
/* ------------------------------------------------------------------ */

struct sbiret
{
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

#define sbi_ecall(ext, fid, a0, a1, a2, a3, a4, a5)         \
    __sbi_ecall((unsigned long)(a0), (unsigned long)(a1),   \
                (unsigned long)(a2), (unsigned long)(a3),   \
                (unsigned long)(a4), (unsigned long)(a5),   \
                (fid), (ext))

/* ------------------------------------------------------------------ */
/* 规范版本号的读取                                                     */
/* ------------------------------------------------------------------ */

extern unsigned long sbi_spec_version;

static inline unsigned long sbi_major_version(void)
{
    return (sbi_spec_version >> SBI_SPEC_VERSION_MAJOR_SHIFT) &
           SBI_SPEC_VERSION_MAJOR_MASK;
}

static inline unsigned long sbi_minor_version(void)
{
    return sbi_spec_version & SBI_SPEC_VERSION_MINOR_MASK;
}

/* ------------------------------------------------------------------ */
/* 控制台                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief 向调试控制台输出一个字符
 * @param[in] ch 要输出的字符
 * @note 走 Legacy 扩展。规范已将 Legacy 整体标记为废弃、建议改用 DBCN，
 *   但这是目前固件下唯一在 console_init() 之前就能用的输出途径，早期启动
 *   阶段和 panic 路径都依赖它。
 */
static inline void sbi_console_putchar(int ch)
{
    register uintptr_t a0 asm("a0") = (uintptr_t)ch;
    register uintptr_t a7 asm("a7") = (uintptr_t)SBI_EXT_0_1_CONSOLE_PUTCHAR;
    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a7)
                 : "memory");
}

/* ------------------------------------------------------------------ */
/* 定时器                                                               */
/* ------------------------------------------------------------------ */

/**
 * @brief 设置下一次定时器中断的绝对时间
 * @param[in] stime_value 绝对时间值（与 time CSR 同一时基）
 * @note 用标准 TIME 扩展而非 Legacy 的 SET_TIMER：后者在 RustSBI 0.4.0 下
 *   是哑的——调用直接返回、mtimecmp 并不会被真正写入，S 态定时器中断永远
 *   不会触发。
 */
static inline void sbi_set_timer(uint64_t stime_value)
{
    sbi_ecall(SBI_EXT_TIME, SBI_EXT_TIME_SET_TIMER, stime_value, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 关机                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief 请求系统关机
 * @note 成功则不返回。用标准 SRST 扩展而非 Legacy 的 SHUTDOWN：后者在
 *   RustSBI 0.4.0 下调用后直接返回，机器根本不会关闭。
 */
static inline void sbi_shutdown(void)
{
    sbi_ecall(SBI_EXT_SRST, SBI_EXT_SRST_RESET,
              SBI_SRST_RESET_TYPE_SHUTDOWN, SBI_SRST_RESET_REASON_NONE,
              0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* 核间中断（IPI）                                                      */
/* ------------------------------------------------------------------ */

/**
 * @brief 向 hart_mask 指定的各 hart 发送核间中断
 * @param[in] hart_mask      位向量，bit N 对应 hart_mask_base + N
 * @param[in] hart_mask_base 位向量起始的 hartid
 * @return SBI 返回码，SBI_SUCCESS 表示已全部送达
 * @note 用标准 IPI 扩展而非 Legacy 的 SEND_IPI，原因同 sbi_set_timer()。
 *   v0.2 起也不再需要 clear_ipi：接收端确认收到的方式是自己清本地的
 *   sip.SSIP，而不是再发一次 SBI 调用。
 */
static inline int sbi_send_ipi(unsigned long hart_mask, unsigned long hart_mask_base)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_IPI, SBI_EXT_IPI_SEND_IPI,
                                  hart_mask, hart_mask_base, 0, 0, 0, 0);
    return (int)ret.error;
}

/* ------------------------------------------------------------------ */
/* Hart 状态管理（HSM）                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief 请求启动指定 hart，使其从 start_addr 开始以 S 态执行
 * @param[in] hartid     目标 hart 的 id
 * @param[in] start_addr 入口物理地址（该 hart 启动时 MMU 是关的）
 * @param[in] priv       透传给目标 hart 的不透明值，会出现在它的 a1 中
 * @return SBI 返回码，SBI_SUCCESS 表示启动请求已被接受
 * @note 异步调用：返回时目标 hart 未必已经开始执行，要确认得轮询
 *   sbi_hsm_hart_status()。
 */
static inline int sbi_hsm_hart_start(unsigned long hartid,
                                     unsigned long start_addr,
                                     unsigned long priv)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
                                  hartid, start_addr, priv, 0, 0, 0);
    return (int)ret.error;
}

/**
 * @brief 查询指定 hart 当前的 HSM 状态
 * @param[in] hartid 目标 hart 的 id
 * @retval <0 SBI 返回的错误码
 * @retval >=0 规范定义的 HSM 状态 id（0=STARTED，1=STOPPED，2=START_PENDING，
 *   3=STOP_PENDING，4=SUSPENDED，5=SUSPEND_PENDING，6=RESUME_PENDING）
 * @note 目标 hart 的状态随时可能因并发的 start/stop/suspend 而变，返回值
 *   只代表调用那一刻的快照。
 */
static inline int sbi_hsm_hart_status(unsigned long hartid)
{
    struct sbiret ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_STATUS,
                                  hartid, 0, 0, 0, 0, 0);
    if (ret.error)
    {
        return (int)ret.error;
    }
    return (int)ret.value;
}

/* ------------------------------------------------------------------ */
/* 固件探测（实现在 sbi.c）                                             */
/* ------------------------------------------------------------------ */

void sbi_init(void);
long sbi_probe_extension(int ext);

#endif /* _SBI_H_ */
