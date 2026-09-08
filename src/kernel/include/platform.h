#ifndef _PLATFORM_H_
#define _PLATFORM_H_

/* 平台相关的编译期常量。
 *
 * ⚠️ **本文件必须保持汇编可包含**：只放 #define，不要放类型、函数原型或任何
 * C 语句——`startup.S` 会 include 它（用它算引导栈的大小与条数）。
 *
 * 平台宏由 makefile 的 `-D $(PLATFORM)` 传入，取值只允许 QEMU / VF2。
 */

#if defined(QEMU)
/* QEMU virt：与 scripts/run.sh 的 -smp 默认值保持一致（改一个要同步改另一个）。
 * 取 4 而不是 2，是为了让日常回归就跑在多核竞争更激烈的配置下——2 核跑不出来的
 * 竞态，4 核更容易撞见，而竞态越早发现越便宜。
 * 两者不一致时会安全降级：cpu_probe_harts() 的扫描带 n < CORE_NUMBER 上限，
 * hart 少了就只用到实际有的那几个，不会越界。 */
#define CORE_NUMBER 4
#elif defined(VF2)
/* JH7110：4×U74 + 1×S7。S7（通常是 hart 0）没有 MMU、不支持 S 态，固件会屏蔽它，
 * 所以能参与调度的只有 4 个 U74——注意这是**逻辑 cpu 数**，不是最大 hartid。 */
#define CORE_NUMBER 4
#else
#error "未知平台：CORE_NUMBER 没有对应取值（PLATFORM 只允许 QEMU / VF2）"
#endif

/* 探测 hart 时扫描的 hartid 上界（不含）。取值只要覆盖目标平台的最大 hartid 即可：
 * QEMU virt 是 0..smp-1，JH7110 是 0..4。扫描靠 SBI 的 HSM hart_status，
 * 不存在的 hartid 会返回错误，多扫几个没有代价。 */
#define MAX_HARTID 8

/* 每个 hart 的引导栈大小。必须是 2 的幂——startup.S 用移位而不是乘法算栈底
 * （那里还没有 C 环境，也不便调用 libgcc 的乘法）。 */
#define BOOT_STACK_SHIFT 15
#define BOOT_STACK_SIZE  (1 << BOOT_STACK_SHIFT)

#endif /* _PLATFORM_H_ */
