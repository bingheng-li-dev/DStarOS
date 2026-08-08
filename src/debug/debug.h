#ifndef _DEBUG_H
#define _DEBUG_H

#define DEBUG 1

#if DEBUG

#define DEBUG_INIT_os_init 0

#define DEBUG_INIT_main 1
#if DEBUG_INIT_main
#define DEBUG_INIT_main_core0 1
#define DEBUG_INIT_main_core1 1
#define DEBUG_INIT_main_bothcore 0
#endif /* DEBUG_INIT_main */

#define DEBUG_TICK 0
#define DEBUG_INTSTACK 0
#define DEBUG_MMU_mm_init 0
#define DEBUG_MMU_mm_alloc 0
#define DEBUG_MMU_mm_dealloc 0
#define DEBUG_MMU_deleteAndReinsert 0
#define DEBUG_MMU_insertAndMerge 0
#define DEBUG_MMU_initMicroPhysicalMemoryPool 0
#define DEBUG_MMU_microAlloc 0
#define DEBUG_LOCK_irq_enable 0
#define DEBUG_PROC_idle 1
#define DEBUG_PROC_createFirstProcIdle 1
#define DEBUG_PROC_proc_init 1
#define DEBUG_PROC_do_fork 1
#define DEBUG_PROC_findProcByPid 1
#define DEBUG_PROC_allocNewProc 1
#define DEBUG_PROC_CTXSTK   1
#define DEBUG_PROC_init     1

#define DEBUG_VMM 1
#if DEBUG_VMM
#define DEBUG_VMM_page_fault_handler 1
#define DEBUG_VMM_self_test 1

#endif /* DEBUG_VMM */

/* 调度器/同步回归测试：置 1 时 init 进程改为运行 run_sched_tests() 再关机，
 * 不再启动用户程序；置 0 走正常的用户程序路径。测试代码在 src/debug/sched_test.c
 * 等文件，入口 run_sched_tests()。 */
#define DEBUG_SCHED_TEST 1

#define DEBUG_TRACK_LINE() printf("DEBUG_TRACK_LINE: %s:%d\n", __FILE__, __LINE__)

#endif /* DEBUG */

#endif /* _DEBUG_H */
