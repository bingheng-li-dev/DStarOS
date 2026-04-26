#include "proc.h"
#include "kmalloc.h"
#include "stringops.h"
#include "errorcode.h"
#include "sched.h"
#include "console.h"
#include "pmm.h"
#include "vfs.h"
#include "cpu.h"

/* List of all processes. */
struct list_head ProcList;
/* Stack of all dealloced pids.In order to alloc these pids again. */
struct list_head PidStack;
/* Idle task. */
pcb_t *TaskIdle = NULL;
/* Init task. */
pcb_t *TaskInit = NULL;
/* Current task. */
pcb_t *TaskCurrent = NULL;
/* Amount of processes. */
uint16_t TaskCount = 0;

/* Alloc a new empty pcb(proc). */
static pcb_t *allocNewProc(void);
/* Dealloc the pcb of a proc. */
static void deallocAProcNotDeeply(pcb_t *pcb);
/* Alloc the kernel stack of a proc. */
static int16_t allocKernelStack(pcb_t *pcb);
/* Dealloc the kernel stack of a proc.Attention that it doesn't dealloc the memory pointed by pointers of the pcb!! */
static int16_t deallocKernelStack(pcb_t *pcb) __attribute__((used));
/* Copy the virtual memory management struct of a proc. */
static int16_t copyProcMm(uint32_t clone_flags, pcb_t *pcb);
/* Copy the stack which is up to the param stack(if stack==0, It means to fork a kernel thread). */
static void copyProcStk(pcb_t *pcb, uintptr_t stack, intstkf_t *regs);
/* Create task idle. */
static pcb_t *createFirstProcIdle(void);
/* Find the pcb of a proc by its pid. */
static pcb_t *findProcByPid(int16_t pid);
/* Alloc a unique pid for process. */
static int16_t allocPidMap(void);
static void deallocPidMap(int16_t pid) __attribute__((used));
/* Kernel's init process which pid is 1. */
static int16_t init(void);
static void fork_out(void);
#if DEBUG_PROC_CTXSTK
static void printCtxStk(ctx_t *ctx);
#endif

char *setProcName(pcb_t *proc, const char *name)
{
    memset(proc->proc_pname, 0, PNAME_MAX_LENGTH);
    return memcpy(proc->proc_pname, name, sizeof(name));
}

/* Remember to recycle memory of name. */
char *getProcName(pcb_t *proc)
{
    char *name = kmalloc(PNAME_MAX_LENGTH);
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->proc_pname, PNAME_MAX_LENGTH);
}

/* @param stack the parent's user stack pointer. if stack==0, It means to fork a kernel thread. */
int16_t do_fork(uint32_t clone_flags, uintptr_t stack, intstkf_t *regs)
{
    if (TaskCount > PROC_MAX_AMOUNT)
    {
        goto f1;
    }
    pcb_t *newProc = allocNewProc();
    if (newProc == NULL)
    {
        goto f1;
    }
    int16_t ret = ENO0_NO_ERROR;
    ret = allocKernelStack(newProc);
    if (ret == ENO1_NOMORE_MEM)
    {
        goto f2;
    }

    copyProcMm(clone_flags, newProc);
    copyProcStk(newProc, stack, regs);

    /* 子进程继承父进程的当前工作目录 */
    newProc->proc_cwd = TaskCurrent->proc_cwd;
    if (newProc->proc_cwd)
    {
        /* 增加 cwd 目录项的引用计数，防止父进程 chdir 后 dentry 被释放 */
        dentry_get_pub(newProc->proc_cwd);
    }

    int16_t pid = ENO3_NOFREE_PID;
    pid = allocPidMap();
    newProc->proc_pid = pid;

#if DEBUG_PROC_do_fork
    printf("do_fork::pid:%d\n", pid);
#endif

    list_add(&(newProc->proc_list_linker), &(ProcList));
    TaskCount = TaskCount + 1;

    return pid;
f1:
    return ENO2_ALLOCPROC_FAILED;
f2:
    deallocAProcNotDeeply(newProc);
    return ENO1_NOMORE_MEM;
}

int16_t do_exit(int16_t error_code)
{
    printf("exit not finished!!\n");
    while (1)
        ;
    return ENO0_NO_ERROR;
}

int16_t createKernelThreadByFork(void *func(void *), void *args, uint32_t clone_flags)
{
    intstkf_t regs;
    memset(&regs, 0, sizeof(intstkf_t));
    regs.x8_s0 = (uint64_t)func;
    regs.x9_s1 = (uint64_t)args;
    /* Make sure that the os is interrupt-enabled and the proc will response interrupt. */
    /* SSTATUS_SPP: Set 1 to make sure S-mode.
     * SSTATUS_SPIE:Set 1 to make sure the interrupt will be enable when goes out of trap.Cause SPIE restores the value of SIE.
     * SSTATUS_SIE: Set 1 to enable global interrupt.Here disable the interrupt in order to simulate a in-trap envirnment. */
    regs.sstatus = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;
    extern void kernel_thread_entry(void);
    regs.sepc = (uint64_t)kernel_thread_entry;
    return do_fork((clone_flags | CLONE_VM), 0, &regs);
}

void proc_init(void)
{
    INIT_LIST_HEAD(&ProcList);
    INIT_LIST_HEAD(&PidStack);
    TaskIdle = createFirstProcIdle();
    if (TaskIdle == NULL)
    {
        printf("Failed to alloc new proc!!\n");
        while (1)
            ;
    }
    TaskCurrent = TaskIdle;
#if DEBUG_PROC_proc_init
    printf("proc_init::TaskCurrent->need_resched:%d TaskIdle->need_resched %d\n", TaskCurrent->need_resched, TaskIdle->need_resched);
    printf("proc_init::TaskCurrent addr:%lx TaskIdle addr %lx\n", (intptr_t)TaskCurrent, (intptr_t)TaskIdle);
#endif
    int16_t id_init = createKernelThreadByFork((void *)init, NULL, 0);
    pcb_t *pcb_init = findProcByPid(id_init);
    const char *name = "init";
    setProcName(pcb_init, name);
    TaskInit = pcb_init;
#if DEBUG_PROC_proc_init
    printf("proc_init::pcb_init pid:%d\n", id_init);
    printf("proc_init::TaskInit addr:%lx\n", (intptr_t)pcb_init);
#endif
    TaskCount = TaskCount + 1;
}

void wakeup(pcb_t *proc)
{
    //未完成的：这里没有重新挂到就绪队列中。
    proc->proc_state = RUNNING;
}

void sleep(void)
{
    //未完成的：这里没有挂到睡眠队列中。
    pcb_t *proc = getCurrentProc();
    proc->proc_state = INTERRUPTIBLE;
    sched();
}

static pcb_t *allocNewProc(void)
{
    pcb_t *pcb = kmalloc(sizeof(pcb_t));
    if (pcb != NULL)
    {
        pcb->kernel_stack = 0;
        /* Only Kernel processes all share the same page dictionary kernel page table(KernelLevel3PageTableFrame). */
        pcb->pageTableBase = convert_pframe2pa(KernelLevel3PageTableFrame);
        memset(&(pcb->proc_context), 0, sizeof(ctx_t));
        pcb->proc_int_stack = NULL;
        pcb->proc_mm = NULL;
        pcb->proc_parent = NULL;
        pcb->proc_pid = ENO3_NOFREE_PID;
        memset(pcb->proc_pname, 0, PNAME_MAX_LENGTH);
        pcb->proc_state = UNINIT;
        pcb->need_resched = false;
        pcb->proc_cwd = NULL;  /* NULL 表示当前工作目录为 VFS 根目录 */
#if DEBUG_PROC_allocNewProc
        printf("allocNewProc::new pcb addr:%lx,sizeof(pcb_t):%ld\n", (intptr_t)pcb, sizeof(pcb_t));
#endif
    }
    return pcb;
}

static void deallocAProcNotDeeply(pcb_t *pcb)
{
    if (pcb != NULL)
    {
        kfree(pcb);
    }
}

static int16_t allocKernelStack(pcb_t *pcb)
{
    phyAddr_t *kernelStackPageAddr = kmalloc(KERNRL_STKSIZE);
    if (kernelStackPageAddr != NULL)
    {
        pcb->kernel_stack = (phyAddr_t)kernelStackPageAddr;
        return ENO0_NO_ERROR;
    }
    return ENO1_NOMORE_MEM;
}

static int16_t deallocKernelStack(pcb_t *pcb)
{
    kfree((void *)(pcb->kernel_stack));
    return ENO0_NO_ERROR;
}

static int16_t copyProcMm(uint32_t clone_flags, pcb_t *pcb)
{
    return ENO0_NO_ERROR;
}

static void copyProcStk(pcb_t *pcb, uintptr_t stack, intstkf_t *regs)
{
    pcb->proc_int_stack = (intstkf_t *)(pcb->kernel_stack + KERNRL_STKSIZE - sizeof(intstkf_t));
    *(pcb->proc_int_stack) = *(regs);
    /* For child process,"fork" returns 0. */
    pcb->proc_int_stack->x10_a0 = 0;
    pcb->proc_int_stack->x2_sp = (stack == 0) ? (uintptr_t)pcb->proc_int_stack : stack;
    pcb->proc_context.x1_ra = (uint64_t)fork_out;
    pcb->proc_context.x2_sp = (uint64_t)pcb->proc_int_stack;
}

static pcb_t *createFirstProcIdle(void)
{
    extern uintptr_t boot_stack_top1; //warning:stack top unsolved

    pcb_t *idle = allocNewProc();
    if (idle != NULL)
    {
        idle->proc_pid = 0;
        idle->kernel_stack = (phyAddr_t)boot_stack_top1;
        idle->proc_state = RUNNING;
        idle->need_resched = true;
        idle->proc_cwd = NULL;  /* idle 进程使用 VFS 根目录 */
        const char *name = "idle";
        setProcName(idle, name);
        TaskCount = TaskCount + 1;
    }
#if DEBUG_PROC_createFirstProcIdle
    printf("createFirstProcIdle::idle->need_resched:%d\n", idle->need_resched);
#endif
    return idle;
}

static pcb_t *findProcByPid(int16_t pid)
{
    if (0 < pid && pid <= PID_MAX_VALUE)
    {
        struct list_head *currentProc;
        pcb_t *currentPcb;
        list_for_each(currentProc, &ProcList)
        {
            currentPcb = list_entry(currentProc, pcb_t, proc_list_linker);
            if (currentPcb->proc_pid == pid)
            {
#if DEBUG_PROC_findProcByPid
                printf("findProcByPid::currentPcb->proc_pid:%d,currentPcb->proc_pname:%s\n", currentPcb->proc_pid, currentPcb->proc_pname);
#endif
                return currentPcb;
            }
        }
    }
    return NULL;
}

static int16_t allocPidMap(void)
{
    static uint16_t lastAlloc = 0;
    int16_t ret = ENO3_NOFREE_PID;
    /* If the list is empty,this loop will be skipped. */
    struct list_head *currentEntry, *tempEntry;
    list_for_each_safe(currentEntry, tempEntry, &PidStack)
    {
        pids_t *cur = list_entry(currentEntry, pids_t, pid_stk_linker);
        list_del_init(currentEntry);
        ret = cur->pid;
        kfree(cur);
        goto f1;
    }
    /* If there is no dealloced pid,alloc a new one. */
    lastAlloc = lastAlloc + 1;
    ret = lastAlloc;
f1:
    return ret;
}

/* A "pids_t" will be alloced when a pid were being dealloced. */
static void deallocPidMap(int16_t pid)
{
    /* Alloc a new "pids_t". */
    pids_t *cur = (pids_t *)kmalloc(sizeof(pids_t));
    cur->pid = pid;
    /* Then add it into the stack list. */
    list_add(&(cur->pid_stk_linker), &PidStack);
}

void idle(void)
{
    while (1)
    {
#if DEBUG_PROC_idle
        printf("idle::TaskCurrent->need_resched:%d TaskIdle->need_resched %d\n", TaskCurrent->need_resched, TaskIdle->need_resched);
        printf("idle::TaskCurrent->proc_pname:%s TaskIdle->proc_pname %s\n", TaskCurrent->proc_pname, TaskIdle->proc_pname);
        printf("idle::TaskCurrent->proc_pid:%d TaskIdle->proc_pid %d\n", TaskCurrent->proc_pid, TaskIdle->proc_pid);
#endif
        if (TaskCurrent->need_resched)
        {
            // sched();
            // TaskCurrent = TaskInit;
#if DEBUG_PROC_CTXSTK
            printCtxStk(&(TaskCurrent->proc_context));
            printCtxStk(&(TaskInit->proc_context));
#endif
            switch_to(&(TaskCurrent->proc_context), &(TaskInit->proc_context));
            while (1)
                ;
        }
    }
}

static int16_t init(void)
{
    printf("Hello!I'm the init process!!\n");
    printf("I'm going away.\n");
    return ENO0_NO_ERROR;
}

static void fork_out(void)
{
    extern void fork_out_asm(intstkf_t * regs);
    printf("fork_out!!\n");
    TaskCurrent = TaskInit;
    fork_out_asm(TaskCurrent->proc_int_stack);
}

#if DEBUG_PROC_CTXSTK
static void printCtxStk(ctx_t *ctx)
{
    printf("\n=================================================================\n");
    printf("  ra       0x%08lx\n", (ctx->x1_ra));
    printf("  sp       0x%08lx\n", (ctx->x2_sp));
    printf("  s0       0x%08lx\n", (ctx->x8_s0));
    printf("  s1       0x%08lx\n", (ctx->x9_s1));
    printf("  s2       0x%08lx\n", (ctx->x18_s2));
    printf("  s3       0x%08lx\n", (ctx->x19_s3));
    printf("  s4       0x%08lx\n", (ctx->x20_s4));
    printf("  s5       0x%08lx\n", (ctx->x21_s5));
    printf("  s6       0x%08lx\n", (ctx->x22_s6));
    printf("  s7       0x%08lx\n", (ctx->x23_s7));
    printf("  s8       0x%08lx\n", (ctx->x24_s8));
    printf("  s9       0x%08lx\n", (ctx->x25_s9));
    printf("  s10      0x%08lx\n", (ctx->x26_s10));
    printf("  s11      0x%08lx\n", (ctx->x27_s11));
    printf("=================================================================\n");
}
#endif
