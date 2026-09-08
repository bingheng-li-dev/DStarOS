#include "cpu.h"
#include "atomic.h"
#include "sbi.h"
#include "sync.h"
#include "console.h"

extern uint64_t cpu_get_core_id_asm(void);
extern void cpu_set_core_id_asm(uint64_t core_id);

/* startup.S 在 _start 里记下的引导核 hartid（未经映射的原始值） */
extern uint64_t boot_hartid_raw;
/* startup.S 里的每核引导栈数组，按逻辑 cpu 号分格 */
extern char boot_stacks[];

static cpu_t cpus[CORE_NUMBER];

/* 逻辑 cpu 号 → hartid。cpu_to_hart[0] 恒为引导核。
 * 这张表是"hartid 与逻辑 cpu 号解耦"的全部内容：内核其余部分一律只认逻辑号，
 * 只有 SBI 调用（HSM 启核、IPI）需要真 hartid，从这里反查。 */
static uint64_t cpu_to_hart[CORE_NUMBER];
static int cpu_present_count = 1;

/**
 * @brief 探测系统里有哪些 hart，建立逻辑 cpu 号 → hartid 的映射
 * @details 靠 SBI 的 HSM `hart_status` 逐个试：不存在或被固件屏蔽的 hartid 会返回
 *   负错误码，跳过即可。这样**不依赖设备树**就能枚举，也天然避开了 VF2 上那颗
 *   不支持 S 态的 S7 监控核（固件不会把它列进自己的 hart mask）。
 *   引导核固定占逻辑号 0，其余按 hartid 升序填。
 * @note 必须在 SBI 可用之后、启动任何从核之前调用。
 */
void cpu_probe_harts(void)
{
    cpu_to_hart[0] = boot_hartid_raw;
    int n = 1;

    for (uint64_t h = 0; h < MAX_HARTID && n < CORE_NUMBER; h++)
    {
        if (h == boot_hartid_raw)
        {
            continue;
        }
        if (sbi_hsm_hart_status(h) < 0)
        {
            continue;
        }
        cpu_to_hart[n] = h;
        n += 1;
    }
    cpu_present_count = n;

    printf("cpu: boot hart %ld -> cpu0; %d cpu(s) present\n",
           (long)boot_hartid_raw, cpu_present_count);
}

int cpu_get_present_count(void)
{
    return cpu_present_count;
}

/**
 * @brief 启动一个从核
 * @param[in] cpu_id 目标**逻辑 cpu 号**（必须 >0，0 是引导核自己）
 * @return SBI 返回码（SBI_SUCCESS 表示已请求启动）
 * @details HSM 约定被启动的 hart 进入时 satp=0、a0=hartid、a1=opaque。
 *   这里把**逻辑 cpu 号当 opaque 传进去**，从核在 startup.S 里直接取 a1 当自己的
 *   逻辑号——否则它只知道自己的 hartid，而 hartid 在目标平台上既不连续也不从 0 起。
 *   `_start` 链接在高 VA，用 kva_to_pa 换算成物理入口传给固件。
 * @note 须在内核页表（含 vmm_kernel_pgd_ppn 与 trampoline 恒等映射）就绪后调用；
 *   移除 trampoline 恒等映射必须等到从核都过了 trampoline 之后（见 init.c 的屏障）。
 */
int cpu_start_secondary_hart(uint16_t cpu_id)
{
    extern char _start[];

    if (cpu_id == 0 || cpu_id >= cpu_present_count)
    {
        return SBI_ERR_INVALID_PARAM;
    }
    mb();
    return sbi_hsm_hart_start(cpu_to_hart[cpu_id],
                              kva_to_pa((virAddr_t)_start),
                              cpu_id);
}

uint64_t cpu_get_core_id(void)
{
    return cpu_get_core_id_asm();
}

void cpu_set_core_id(uint64_t core_id)
{
    /* 存的是逻辑 cpu 号。原来这里有个 `& 0x1` 的掩码，把系统硬锁死在 2 核——
     * 逻辑号一旦到 2 就被截断成 0，两个核会共用同一个 cpu_t。 */
    cpu_set_core_id_asm(core_id);
}

cpu_t *cpu_get_current(void)
{
    return &cpus[cpu_get_core_id()];
}

cpu_t *cpu_get_by_index(uint16_t index)
{
    return &cpus[index];
}

/**
 * @brief 逻辑 cpu 号对应的引导栈顶
 * @details 与 startup.S 的 setup_boot_stack 必须算得一致：栈由高向低增长，
 *   逻辑 cpu N 用 [N*SIZE, (N+1)*SIZE) 这一格，栈顶取该格的上界。
 */
uintptr_t cpu_boot_stack_top(uint16_t cpu_id)
{
    return (uintptr_t)boot_stacks + ((uintptr_t)cpu_id + 1) * BOOT_STACK_SIZE;
}

/**
 * @brief 给指定逻辑 cpu 发一次核间中断（S 态软件中断）
 * @param[in] cpu_id 目标逻辑 cpu 号
 * @details 用于把正在 wfi 休眠的核踢醒，让它回到 idle() 循环重新看一眼就绪队列。
 *   目标核收到 IRQ_S_SOFT 后只需清本地 sip.SSIP（见 trap.c），中断本身不用做
 *   任何调度决策——真正的调度检查在 idle() 循环里做，中断只是"唤醒"这一下。
 */
void cpu_send_ipi(uint16_t cpu_id)
{
    if (cpu_id >= cpu_present_count)
    {
        return;
    }
    sbi_send_ipi(BIT(cpu_to_hart[cpu_id]), 0);
}
