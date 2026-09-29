/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "cpu.h"
#include "atomic.h"
#include "sbi.h"
#include "sync.h"
#include "console.h"
#include "fdt.h"
#include "stringops.h"
#include "startup.h"

static cpu_t cpus[CORE_NUMBER];

/* 逻辑 cpu 号 → hartid。cpu_to_hart[0] 恒为引导核。
 * 这张表是"hartid 与逻辑 cpu 号解耦"的全部内容：内核其余部分一律只认逻辑号，
 * 只有 SBI 调用（HSM 启核、IPI）与 PLIC 的 context 编号需要真 hartid，从这里反查。 */
static uint64_t cpu_to_hart[CORE_NUMBER];
static int cpu_present_count = 1;

/**
 * @brief 判断 riscv,isa 字符串是否表明这颗核支持 S 态
 * @param[in] isa 设备树 cpu 节点的 "riscv,isa" 属性值
 * @details 只看单字母扩展序列（第一个 '_' 之前）里有没有 's'。
 *   整串扫是错的：多字母扩展名里带 s 的比比皆是（zicsr / sstc / svadu），
 *   那样判据会永远返回"支持"，等于没写。
 *
 *   判据只对老规范的 isa 字符串成立，用序列里有没有 'u' 来识别：
 *     老规范把特权级当扩展写，VF2 实测 U74 是 "rv64imafdcbsux"、S7 是 "rv64imacu"；
 *     新规范不再写 s/u，QEMU 8.2 实测是 "rv64imafdch_zicbom_..._sstc_svadu"，
 *     这时 isa 提供不了任何特权级信息。
 *   所以不含 'u' 就直接认为可用，否则 QEMU 上全部核会被排除、静默退化成单核。
 * @note 这是启发式，不是规范保证。它成立的前提是"写了 u 就会写 s"，
 *   目前两类字符串都符合，但没有哪份规范强制这一点。
 */
static bool isa_has_supervisor(const char *isa)
{
    bool has_s = false;
    bool has_u = false;

    for (const char *p = isa; *p != '\0' && *p != '_'; p++)
    {
        if (*p == 's')
        {
            has_s = true;
        }
        else if (*p == 'u')
        {
            has_u = true;
        }
    }
    return has_u ? has_s : true;
}

/**
 * @brief 探测系统里有哪些 hart，建立逻辑 cpu 号 → hartid 的映射
 * @details 遍历设备树 /cpus 下的 cpu 节点，从 `reg` 取 hartid、按 `riscv,isa`
 *   判断能否跑 S 态，再用 HSM `hart_status` 确认固件愿意启动它。
 *   引导核固定占逻辑号 0，其余按设备树里的顺序填。
 *
 *   不能只靠 HSM 枚举：VF2 的 OpenSBI 把那颗不支持 S 态的 S7 监控核（hart 0）
 *   也列进了 domain0（`Domain0 HARTs: 0*,1*,2*,3*,4*`），照着 HSM 的结果启动它
 *   就是让一颗没有 MMU 的核去执行 csrw satp。
 *
 *   也不能靠 mmu-type / status / compatible：那块板子的 U-Boot 控制 DTB 里
 *   S7 这三项分别写着 "riscv,sv39" / "okay" / "sifive,u74-mc"，全是从 U74 抄来的，
 *   一条都不能信。riscv,isa 是唯一如实反映硬件的字段。
 * @note 必须在 fdt_init() 之后、MMU 开启之前调用——DTB 不在内核偏移映射内。
 *   读不到设备树时退回单核：引导核既然执行到了这里，它必然支持 S 态，
 *   这个方向的失败是安全的，而猜错了去启动 S7 则是随机死机。
 */
void cpu_probe_harts(void)
{
    cpu_to_hart[0] = boot_hartid_raw;
    cpu_present_count = 1;

    const void *cpus_node = fdt_find_node("/cpus");
    if (cpus_node == NULL)
    {
        printf("cpu: %s -- running single core on boot hart %ld\n",
               fdt_is_available() ? "dtb has no /cpus" : "no usable dtb",
               (long)boot_hartid_raw);
        return;
    }

    int n = 1;
    for (const void *cpu = fdt_first_subnode(cpus_node);
         cpu != NULL;
         cpu = fdt_next_subnode(cpu))
    {
        uint64_t hartid = 0;
        const char *dtype = fdt_prop_str(cpu, "device_type");

        /* /cpus 底下不只有 cpu 节点，还可能挂 cpu-map 之类的拓扑描述 */
        if (dtype == NULL || strncmp(dtype, "cpu", 4) != 0)
        {
            continue;
        }
        if (!fdt_prop_u64(cpu, "reg", &hartid))
        {
            continue;
        }

        /* status 与 isa 是互补的两条判据，各自覆盖一类设备树，都要查：
         *   这块板子的 U-Boot 控制 DTB 用老规范写 isa，S7 是 "rv64imacu"，
         *   但它的 status 撒谎写成 "okay"——只查 status 会漏；
         *   上游 Linux 的 jh7110 dtsi 用新规范写 isa（"rv64imac_zba_zbb"，无 u），
         *   isa 判据对它失效，靠的正是 status = "disabled"——只查 isa 会漏。
         * 设备树规范里 status 缺失等同 "okay"，另外 "reserved" 表示轮不到 OS 用。 */
        const char *status = fdt_prop_str(cpu, "status");
        if (status != NULL && strncmp(status, "okay", 5) != 0 &&
            strncmp(status, "ok", 3) != 0)
        {
            printf("cpu: hart %ld status \"%s\", skipped\n", (long)hartid, status);
            continue;
        }

        const char *isa = fdt_prop_str(cpu, "riscv,isa");
        if (isa == NULL || !isa_has_supervisor(isa))
        {
            printf("cpu: hart %ld isa \"%s\" -- no S-mode, skipped\n",
                   (long)hartid, isa != NULL ? isa : "(missing)");
            continue;
        }
        if (hartid == boot_hartid_raw || n >= CORE_NUMBER)
        {
            continue;
        }
        if (sbi_hsm_hart_status(hartid) < 0)
        {
            printf("cpu: hart %ld not startable via HSM, skipped\n", (long)hartid);
            continue;
        }
        cpu_to_hart[n] = hartid;
        n += 1;
    }
    cpu_present_count = n;

    printf("cpu: %d cpu(s):", cpu_present_count);
    for (int i = 0; i < cpu_present_count; i++)
    {
        printf(" cpu%d=hart%ld", i, (long)cpu_to_hart[i]);
    }
    printf("\n");
}

/**
 * @brief 探测到的可用 cpu 数
 */
int cpu_get_present_count(void)
{
    return cpu_present_count;
}

/**
 * @brief 逻辑 cpu 号 → hartid
 * @param[in] cpu_id 逻辑 cpu 号，必须小于 cpu_get_present_count()
 * @return 该 cpu 的真 hartid
 * @note 内核其余部分只认逻辑号；要 hartid 的只有直接和固件/硬件打交道的地方
 *   （SBI 调用、按 hart 排列的 PLIC context）。
 */
uint64_t cpu_get_hartid(int cpu_id)
{
    return cpu_to_hart[cpu_id];
}

/**
 * @brief 启动一个从核
 * @param[in] cpu_id 目标逻辑 cpu 号（必须 >0，0 是引导核自己）
 * @return SBI 返回码（SBI_SUCCESS 表示已请求启动）
 * @details HSM 约定被启动的 hart 进入时 satp=0、a0=hartid、a1=opaque。
 *   这里把逻辑 cpu 号当 opaque 传进去，从核在 startup.S 里直接取 a1 当自己的
 *   逻辑号——否则它只知道自己的 hartid，而 hartid 在目标平台上既不连续也不从 0 起。
 *   `_start` 链接在高 VA，用 kva_to_pa 换算成物理入口传给固件。
 * @note 须在内核页表（含 vmm_kernel_pgd_ppn 与 trampoline 恒等映射）就绪后调用；
 *   移除 trampoline 恒等映射必须等到从核都过了 trampoline 之后（见 init.c 的屏障）。
 */
int cpu_start_secondary_hart(uint16_t cpu_id)
{
    if (cpu_id == 0 || cpu_id >= cpu_present_count)
    {
        return SBI_ERR_INVALID_PARAM;
    }
    mb();
    return sbi_hsm_hart_start(cpu_to_hart[cpu_id],
                              kva_to_pa((virAddr_t)_start),
                              cpu_id);
}

/**
 * @brief 本 hart 的逻辑 cpu 号（读 tp）
 */
uint64_t cpu_get_core_id(void)
{
    return cpu_get_core_id_asm();
}

/**
 * @brief 把本 hart 的逻辑 cpu 号写进 tp
 */
void cpu_set_core_id(uint64_t core_id)
{
    cpu_set_core_id_asm(core_id);
}

/**
 * @brief 本 hart 的 cpu_t
 */
cpu_t *cpu_get_current(void)
{
    return &cpus[cpu_get_core_id()];
}

/**
 * @brief 按逻辑 cpu 号取 cpu_t
 */
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
