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

struct dentry;
typedef struct dentry dentry_t;
struct file;
typedef struct file file_t;
struct sched_class;

#define SCHED_NORMAL 0 /* CFS 类：nice 加权 vruntime */
#define SCHED_FIFO 1   /* RT 类：固定优先级，不轮转 */
#define SCHED_RR 2     /* RT 类：固定优先级，同优先级时间片轮转 */

#define PNAME_MAX_LENGTH 64
#define KERNEL_STACKPSIZE 1
#define KERNRL_STKSIZE KERNEL_STACKPSIZE *PGSIZE
#define PID_MAX_VALUE (((int16_t)1 << 15) - 2) /* 0 <= PID <= PID_MAX_VALUE*/
#define PROC_MAX_AMOUNT (PID_MAX_VALUE / 2)    /* 1(idle) <= task_count <= PROC_MAX_AMOUNT */
#define NOFILE 16

#define CLONE_VM 0x00000100      /* Child process will share the same virtual memory space with it's parent. */
#define CLONE_FS 0x00000200      /* Child process will share the same file system info with it's parent. */
#define CLONE_FILES 0x00000400   /* Child process will share the same opened files with it's parent. */
#define CLONE_SIGHAND 0x00000800 /* Child process will share the same signal handle program with it's parent. */

typedef enum proc_status sta_t;
typedef struct proc_context ctx_t;
typedef struct proc_control_block pcb_t;
typedef struct proc_pid_map pids_t;

enum proc_status
{
    RUNNING = 0,     /* READY和RUNNING统称为RUNNING状态。 */
    UNINTERRUPTIBLE, /*  处于等待队伍中，等待资源有效时唤醒且不可以被中断唤醒。 */
    INTERRUPTIBLE,   /*  处于等待队伍中，等待资源有效时唤醒且可以被中断唤醒。 */
    ZOMBIE,
    UNINIT,
};

struct proc_context
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
    uint64_t satp;
};

struct proc_control_block
{
    uint16_t proc_pid;
    char proc_pname[PNAME_MAX_LENGTH + 1];
    sta_t proc_state;
    ctx_t proc_context;
    uintptr_t kernel_stack;
    pcb_t *proc_parent;
    intstkf_t *proc_int_stack;
    mm_t *proc_mm;
    volatile bool need_resched;
    dentry_t   *proc_cwd;                /* 当前工作目录的目录项；NULL 表示使用 VFS 根目录 */
    int16_t proc_exit_code;              /* 退出码；ZOMBIE 期间保存，待父进程 do_wait 收割 */
    struct list_head proc_children;      /* 子进程链表头——子进程以 proc_sibling_linker 挂入 */
    struct list_head proc_sibling_linker;/* 本进程挂入父进程 proc_children 节点 */
    file_t *proc_fds[NOFILE];            /* 文件描述符fd表 */
    uint8_t proc_fd_flags[NOFILE];       /* 每个 fd 的标志位 */

    const struct sched_class *proc_sched_class; /* 本任务归属的调度类 */
    int proc_policy;                            /* SCHED_NORMAL / SCHED_FIFO / SCHED_RR */
    /* 是否在就绪队列中。enqueue/dequeue 据此做幂等保护：
     * 对一个已入队（甚至正在运行）的任务再次 enqueue，会把同一个 rb_node
     * 挂到红黑树两处而损坏树结构——do_exit 里 wakeup(父进程) 时父进程往往并未睡眠，
     * 这条路径一定会触发。反向地，rb_erase 一个不在树中的节点是未定义行为。 */
    bool proc_on_rq;
    /* true 表示本任务正占用某个 hart 执行（或正在 switch_to 保存上下文的途中），
     * 此时它的 proc_context 尚未写完，绝不能被另一个 hart 挑走换上。
     * 由 sched_set_current() 置位、sched_finish_switch() 在 switch_to 完成后清零，
     * 全程在 run_queue.lock 保护下读写。 */
    volatile bool proc_on_cpu;

    /* ==================== CFS调度相关 ==================== */
    int proc_nice;               /* nice 值 [-20, 19]，默认 0 */
    uint32_t proc_weight;        /* 由 nice 派生的权重，nice=0 时为 SCHED_NICE_0_WEIGHT */
    uint64_t proc_vruntime;      /* 加权虚拟运行时间，单位同 sched_now() */
    uint64_t proc_exec_start;    /* 上次结算（换入/每个 tick）的时刻，用于算本次 delta；每次结算都刷新 */
    uint64_t proc_sum_exec_runtime_prev; /* 换入 CPU 那一刻对 proc_sum_exec_runtime 拍的快照，只在换入时写 */
    uint64_t proc_sum_exec_runtime;      /* 累计执行时间 */
    struct rb_node proc_rbtree_node;     /* 挂入 cfs_rq.tasks 的节点 */

    /* ==================== RT调度相关 ==================== */
    uint8_t proc_rt_priority;            /* 数值越大优先级越高（FreeRTOS 约定） */
    struct list_head proc_rt_linker;     /* 挂入 rt_rq.ready_lists[prio] 的节点 */

    /* 一个节点不能同时挂入两条链 */
    struct list_head proc_list_linker;   /* 挂入全局 proc_list 的节点 */
    struct list_head proc_wait_linker;   /* 挂入等待队列（信号量 / wait）的节点 */

    /* ==================== 定时唤醒相关 ==================== */
    uint64_t proc_wake_tick;             /* 到期时刻（tick_get_os_tick() 单位），仅挂在 sleeping_tasks 期间有效 */
    struct list_head proc_timer_linker;  /* 挂入 sleeping_tasks 排序链表的节点 */
};

struct proc_pid_map
{
    int16_t pid;
    struct list_head pid_stk_linker;
};

extern void switch_to(ctx_t *from, ctx_t *to);

char *set_proc_name(pcb_t *proc, const char *name);
char *get_proc_name(pcb_t *proc);
int16_t do_fork(uint32_t clone_flags, uintptr_t stack, intstkf_t *regs);
void do_exit(int16_t error_code) __attribute__((noreturn));
int16_t do_wait(int16_t pid, int *status);
int do_exec(intstkf_t *sp, const char *path);
int16_t create_kernel_thread_by_fork(void *func(void *), void *args, uint32_t clone_flags);
/* 按 pid 查找 pcb（find_proc_by_pid 的公开包装）；未找到返回 NULL。 */
pcb_t *proc_find_by_pid(int16_t pid);
void proc_init(void);
pcb_t *proc_get_current(void);

int proc_fd_alloc(void);
/* 从 from 起找最小空闲 fd（fcntl F_DUPFD 用）；proc_fd_alloc() 等价于 from=0 */
int proc_fd_alloc_from(int from);
file_t *proc_fd_get(int fd);
int proc_fd_install(int fd, file_t *f);
int proc_fd_close(int fd);
int proc_fd_copy(pcb_t *dst, pcb_t *src);
void proc_fd_close_all(pcb_t *p);
int proc_install_stdio(void);
/* 设置某个 fd 的标志位；fd 越界或槽位未打开时静默忽略 */
void proc_fd_set_flags(int fd, uint8_t flags);
/* 读取某个 fd 的标志位；fd 越界或槽位未打开时返回 0 */
uint8_t proc_fd_get_flags(int fd);
/* execve 成功、旧地址空间已销毁、不再可能回滚之后调用：关闭所有带 FD_CLOEXEC 的 fd */
void proc_fd_close_on_exec(pcb_t *p);

/* Kernel's idle process which pid is 0. */
void idle(void) __attribute__((noreturn));
void enter_user_mode(virAddr_t entry, virAddr_t ustack) __attribute__((noreturn));

#endif
