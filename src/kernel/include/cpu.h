/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "proc.h"
#include "platform.h"   /* CORE_NUMBER / BOOT_STACK_* */

typedef struct os_cpu cpu_t;

struct os_cpu
{
    pcb_t *current_proc;
    pcb_t *idle_proc;        /* 本 hart 自己的 idle 任务 */
    /* 时钟停摆护栏（见 sched_schedule）：上次见到的 tick，与此后连续调度而 tick 没动的次数 */
    uint64_t sched_last_tick;
    uint32_t sched_same_tick;
    pcb_t *prev_proc;        /* 刚被 switch_to 换下的任务，由换上来的执行流负责清它的 proc_on_cpu */
    ctx_t *ctx;
    uint64_t tick;           /* 当前CPU的tick。 */
};

/* 注意：以下接口里的 "core id" 一律是逻辑 cpu 号（引导核恒为 0、连续编号），
 * 不是 hartid。两者在 QEMU 上碰巧相等，在 VF2 上不相等——JH7110 的 hart 0 是
 * 不支持 S 态的 S7 监控核，引导核是某个 U74。需要真 hartid 的只有 SBI 调用与 PLIC context，
 * 经 cpu_get_hartid() 反查。 */
void cpu_probe_harts(void);
int cpu_get_present_count(void);
uint64_t cpu_get_hartid(int cpu_id);
int cpu_start_secondary_hart(uint16_t cpu_id);
uint64_t cpu_get_core_id(void);
void cpu_set_core_id(uint64_t core_id);
cpu_t *cpu_get_current(void);
cpu_t *cpu_get_by_index(uint16_t index);
void cpu_send_ipi(uint16_t cpu_id);
/* 逻辑 cpu 号 → 该核的引导栈顶（供 idle 任务登记 kernel_stack 用） */
uintptr_t cpu_boot_stack_top(uint16_t cpu_id);

#endif /* _CPU_H_ */
