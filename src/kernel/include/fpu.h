/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _FPU_H_
#define _FPU_H_

struct proc_control_block;

/* 每个 hart 启动时把 sstatus.FS 从 Off 打开到 Initial。
 * **必须在任何进程可能执行浮点指令之前调用**，也包括内核自己——fpu_save/fpu_restore
 * 里的 fsd/fld 同样受 FS 管，FS=Off 时它们自己就会触发非法指令。 */
void fpu_init(void);

/* 把当前 hart 的 32 个浮点寄存器与 fcsr 存进 pcb / 从 pcb 恢复。
 * 只在真正发生任务切换时调用（见 sched.c 的两个调用点）：**没切换却调 restore，
 * 等于拿上次换出时的旧值盖掉此刻活着的寄存器**。 */
void fpu_save(struct proc_control_block *p);
void fpu_restore(struct proc_control_block *p);

#endif /* _FPU_H_ */
