#include "cpu.h"
#include "atomic.h"
#include "sbi.h"
#include "sync.h"

extern uint64_t cpu_get_core_id_asm(void);
extern void cpu_set_core_id_asm(uint64_t core_id);

static cpu_t cpus[CORE_NUMBER];

/**
 * @brief 启动 hart 1
 * @return SBI 返回码（SBI_SUCCESS 表示已请求启动）
 * @details RustSBI 0.4.0 用 HSM 扩展把从核停在 STOPPED 态，只把 hart 0 重定向到内核入口；
 *   老的 `sbi_send_ipi` + `core2Enabled` 轮询根本唤不醒 STOPPED 的从核。必须用
 *   HSM `sbi_hart_start` 让 hart 1 从**物理入口** `_start` 起跑（HSM 约定进入时 satp=0、
 *   a0=hartid、a1=priv）。`_start` 链接在高 VA，用 `kva_to_pa` 换算成 PA(0x80200000) 传入。
 * @note 须在内核页表（含 vmm_kernel_pgd_ppn 与 trampoline 恒等映射）就绪后调用；
 *   移除 trampoline 恒等映射必须等到 hart 1 过了 trampoline 之后（见 init.c 的 hart1_up 屏障）。
 */
int cpu_start_secondary_hart(void)
{
    extern char _start[];
    mb();
    return sbi_hsm_hart_start(1, kva_to_pa((virAddr_t)_start), 0);
}

uint64_t cpu_get_core_id(void)
{
    return cpu_get_core_id_asm();
}

void cpu_set_core_id(uint64_t core_id)
{
    return cpu_set_core_id_asm(core_id & 0x1);
}

cpu_t *cpu_get_current(void)
{
    return &cpus[cpu_get_core_id()];
}

cpu_t *cpu_get_by_index(uint16_t index)
{
    return &cpus[index];
}
