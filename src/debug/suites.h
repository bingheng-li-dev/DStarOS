/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _SUITES_H_
#define _SUITES_H_

void debug_suite_run(void);

void run_sched_tests(void);
void run_slab_tests(void);
void run_dcache_tests(void);
void vfs_test(void);

/* run_sched_tests 聚合的子套件 */
void sync_sem_wakeup_test(void);
void sync_mutex_test(void);
void rt_preempt_cfs_test(void);
void rt_rr_rotation_test(void);
void waitq_single_wakeup_test(void);
void waitq_broadcast_test(void);
void run_pipe_tests(void);

/* 调度类子套件共用的断言、收割与起跑闸门（sched_test.c） */
void sched_test_check(const char *name, int cond);
int sched_test_reap_all(void);
void sched_test_gate_init(void);
void sched_test_gate_wait(void);
void sched_test_gate_release(void);

#endif /* _SUITES_H_ */
