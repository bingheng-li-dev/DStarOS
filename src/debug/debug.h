#ifndef _DEBUG_H
#define _DEBUG_H

#define DEBUG 1

#if DEBUG

#define DEBUG_INIT_os_init 1
#define DEBUG_INIT_MAIN 0
#define DEBUG_TICK 1
#define DEBUG_INTSTACK 0
#define DEBUG_MMU_mm_init 0
#define DEBUG_MMU_mm_alloc 0
#define DEBUG_MMU_mm_dealloc 0
#define DEBUG_MMU_kernelPa2Va_IdentityMapping 0
#define DEBUG_MMU_deleteAndReinsert 0
#define DEBUG_MMU_insertAndMerge 0
#define DEBUG_MMU_initMicroPhysicalMemoryPool 0
#define DEBUG_MMU_microAlloc 1
#define DEBUG_LOCK_irq_enable 1
#define DEBUG_PROC_idle 1
#define DEBUG_PROC_createFirstProcIdle 1
#define DEBUG_PROC_proc_init 1
#define DEBUG_PROC_do_fork 1
#define DEBUG_PROC_findProcByPid 1
#define DEBUG_PROC_allocNewProc 1
#define DEBUG_PROC_CTXSTK   1

#endif

#endif