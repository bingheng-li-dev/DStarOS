#ifndef _CPU_H_
#define _CPU_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "proc.h"

/* 1 <= CORE_NUMBER. */
#define CORE_NUMBER 2

typedef struct os_cpu cpu_t;

struct os_cpu
{
    pcb_t *current_proc;
    pcb_t *idle_proc;        /* 本 hart 自己的 idle 任务 */
    pcb_t *prev_proc;        /* 刚被 switch_to 换下的任务，由换上来的执行流负责清它的 proc_on_cpu */
    ctx_t *ctx;
    int irq_disable_nesting; /* 中断请求屏蔽嵌套数量。 */
    bool intr_disable_state; /* 当前中断屏蔽开启/关闭状态(true/false)。eg:如果中断处于关闭状态时"intr_disable_state"为true。 */
    uint64_t tick;           /* 当前CPU的tick。 */
};

int cpu_start_secondary_hart(void);
uint64_t cpu_get_core_id(void);
void cpu_set_core_id(uint64_t core_id);
cpu_t *cpu_get_current(void);
cpu_t *cpu_get_by_index(uint16_t index);
void cpu_send_ipi(uint64_t hart_id);

#endif