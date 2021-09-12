#ifndef _PROC_H_
#define _PROC_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "list.h"
#include "rbtree.h"
#include "rbtree_augmented.h"
#include "memtype.h"
#include "trap.h"
#include "vmm.h"

#define PNAME_MAX_LENGTH 64
#define KERNEL_STACKPSIZE 1
#define KERNRL_STKSIZE KERNEL_STACKPSIZE *PGSIZE
#define PID_MAX_VALUE (((int16_t)1 << 15) - 2) /* 0 <= PID <= PID_MAX_VALUE*/
#define PROC_MAX_AMOUNT (PID_MAX_VALUE / 2)    /* 1(idle) <= TaskCount <= PROC_MAX_AMOUNT */

#define CLONE_VM 0x00000100      /* Child process will share the same virtual memory space with it's parent. */
#define CLONE_FS 0x00000200      /* Child process will share the same file system info with it's parent. */
#define CLONE_FILES 0x00000400   /* Child process will share the same opened files with it's parent. */
#define CLONE_SIGHAND 0x00000800 /* Child process will share the same signal handle program with it's parent. */

typedef enum statusOfProcess sta_t;
typedef struct contextOfProcess ctx_t;
typedef struct controlBlockOfProcess pcb_t;
typedef struct pidMap pids_t;

enum statusOfProcess
{
    RUNNING = 0,     /* READY和RUNNING统称为RUNNING状态。 */
    UNINTERRUPTIBLE, /*  处于等待队伍中，等待资源有效时唤醒且不可以被中断唤醒。 */
    INTERRUPTIBLE,   /*  处于等待队伍中，等待资源有效时唤醒且可以被中断唤醒。 */
    ZOMBIE,
    UNINIT,
};

struct contextOfProcess
{
    uint64_t x1_ra;
    uint64_t x2_sp;
    uint64_t x8_s0;
    uint64_t x9_s1;
    uint64_t x18_s2;
    uint64_t x19_s3;
    uint64_t x20_s4;
    uint64_t x21_s5;
    uint64_t x22_s6;
    uint64_t x23_s7;
    uint64_t x24_s8;
    uint64_t x25_s9;
    uint64_t x26_s10;
    uint64_t x27_s11;
};

struct controlBlockOfProcess
{
    uint16_t proc_pid;
    char proc_pname[PNAME_MAX_LENGTH + 1];
    sta_t proc_state;
    ctx_t proc_context;
    uintptr_t pageTableBase;
    uintptr_t kernel_stack;
    pcb_t *proc_parent;
    intstkf_t *proc_int_stack;
    mm_t *proc_mm;
    volatile bool need_resched;
    struct list_head proc_list_linker;
    struct rb_node proc_rbtree_node;
};

struct pidMap
{
    int16_t pid;
    struct list_head pid_stk_linker;
};

extern void switch_to(ctx_t *from, ctx_t *to);

char *setProcName(pcb_t *proc, const char *name);
char *getProcName(pcb_t *proc);
int16_t do_fork(uint32_t clone_flags, uintptr_t stack, intstkf_t *regs);
int16_t do_exit(int16_t error_code);
int16_t createKernelThreadByFork(void *func(void *), void *args, uint32_t clone_flags);
void proc_init(void);
/* Kernel's idle process which pid is 0. */
void idle(void) __attribute__((noreturn));
void wakeup(pcb_t* proc);

#endif
