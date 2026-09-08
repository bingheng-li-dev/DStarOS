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
#include "signal.h"

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

/* 内核栈**最高的这些字节不作栈用**，留作 per-hart 记账区：[栈顶, 栈顶+8) 存
 * "本任务当前跑在哪个 hart 上"。`trap_return` 返回 U 态前写进去，`trap_entry`
 * 从 U 态进来时读回 `tp`。
 *
 * 为什么需要它：内核拿 `tp` 当 hart 号用（`cpu_get_core_id_asm()` 直接读 tp，
 * `cpu_get_current()` = `&cpus[tp]`），而 **`tp` 在 RISC-V 上是用户可写的普通寄存器**
 * ——musl 的 `__set_thread_area` 就是一条 `mv tp, a0`，纯寄存器写、不走 syscall，
 * 内核根本无从察觉。用户一旦把 tp 改成自己的 TLS 指针，之后每次陷入内核，
 * `cpus[tp]` 就是野指针。这个不变式此前只是靠"手写的用户程序碰巧不碰 tp"维持着。
 *
 * 取 16 而不是 8：RISC-V psABI 要求栈指针 16 字节对齐，保留区大小必须是 16 的倍数，
 * 否则下移后的栈顶会破坏对齐。 */
#define KSTACK_RESERVED 16

/* 供 trap 使用的内核栈顶（sscratch 存的就是它）。注意**不是**分配区的末端：
 * 最高 KSTACK_RESERVED 字节被保留区占着。 */
#define PROC_KSTACK_TOP(pcb) ((uintptr_t)((pcb)->kernel_stack + KERNRL_STKSIZE - KSTACK_RESERVED))
#define PID_MAX_VALUE (((int16_t)1 << 15) - 2) /* 0 <= PID <= PID_MAX_VALUE*/
#define PROC_MAX_AMOUNT (PID_MAX_VALUE / 2)    /* 1(idle) <= task_count <= PROC_MAX_AMOUNT */
/* fd 表槽位数。16 是阶段 2 定的，够我们自己的测试程序用；**对 ash 不够**——
 * 它处理重定向的核心动作 savefd() 是 fcntl(fd, F_DUPFD, 10)，刻意把要保存的 fd
 * 挪到 10 以上避开用户可能用到的 0~9，16 个槽位减去 stdio 之后 10 以上只剩 6 个。
 * 64 是"多重重定向的管道够用"与"PCB 别无谓变胖"之间的保守中点；真要到 Linux 的
 * 1024 得把 fd 表改成动态分配，那是另一件事。 */
#define NOFILE 64

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

    /* ==================== 信号相关 ==================== */
    sighand_t *proc_sighand;             /* 信号处理表；NULL = 内核线程，不接收信号 */
    sigset_t proc_sig_pending;           /* 挂起集，位 n-1 对应信号 n */
    sigset_t proc_sig_mask;              /* 屏蔽集 */
    uint64_t proc_syscall_orig_a0;       /* 本次 syscall 进来时的 a0，SA_RESTART 重启时要还原 */
    uint8_t proc_exit_sig;               /* 非 0 = 被该信号杀死，do_wait 据此编码 wait status */
    int16_t proc_pgid;                   /* 进程组，fork 继承、exec 不变；^C 打给前台进程组 */

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
    /* sched_schedule() 里 acquire run_queue.lock 拿到的中断 key。这把锁是**接力**释放的
     * （见 sched_finish_switch），acquire 与 release 由不同执行流完成、任务还可能换到
     * 另一个 hart 上才被换回，所以 key 绝不能存在 per-CPU 的槽里——存进 pcb 让它随任务
     * 走才配得上对。新建任务要初始化成 true（"之前中断是开的"），它第一次被换上时
     * fork_out 里的 sched_finish_switch 靠这个值把中断打开。 */
    bool     proc_rq_key;
    uint64_t proc_vruntime;      /* 加权虚拟运行时间，单位同 sched_now() */
    uint32_t proc_vruntime_rem;  /* 上次换算 vruntime 时除不尽的余数，见 fair_update_curr()
                                  * 里的说明；恒 < proc_weight，改 nice 时清零 */
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
    /* 到期时刻（ktime_get_ns() 时基的绝对纳秒），仅挂在 sleeping_tasks 期间有效。
     * 用纳秒而不是 tick 计数，不是为了到期精度（检查点仍是每 tick 一次），而是
     * nanosleep 被信号打断时必须回填准确的剩余时间，tick 粒度下只能给出 ±5 ms。 */
    uint64_t proc_wake_time_ns;
    /* 挂入 sleeping_tasks 排序链表的节点。**空链表状态（list_empty）是
     * sched_timer_remove() 幂等的唯一判据**，所以入链前、摘链后都必须维护它。 */
    struct list_head proc_timer_linker;

    /* ==================== ITIMER_REAL 定时器 ==================== */
    uint64_t proc_alarm_expire_ns;       /* 到期的绝对时刻，0 = 未装定时器 */
    uint64_t proc_alarm_interval_ns;     /* 周期，0 = 单次 */
    /* 挂入 ktime.c 的 alarm_list 的节点。与 proc_timer_linker 分开是必须的：
     * 一个进程可以同时睡在 nanosleep 里、又装着定时器，共用一个节点等于挂两条链。 */
    struct list_head proc_alarm_linker;

    /* ==================== 杂项 ==================== */
    uint16_t proc_umask;                 /* 文件创建掩码，fork 继承、exec 保留 */
    uint64_t proc_clear_child_tid;       /* set_tid_address 存下的用户指针；不做退出时的 futex 唤醒 */
    uint64_t proc_sum_exec_runtime_children; /* 已收割子进程的累计执行时间之和，times() 的 tms_cutime */

    /* ==================== 浮点上下文 ==================== */
    /* 32 个 f 寄存器 + fcsr，切换任务时无条件存取（见 fpu.c 里为什么不做惰性保存）。
     * 为它们腾出这 264 字节是 BusyBox 逼出来的：musl 在 lp64d 下的 setjmp/longjmp
     * 会无条件存取 fs0~fs11，而 ash 的异常机制就建立在 setjmp 上——没有 FP 上下文，
     * ash 起来的第一条 setjmp 就会被 SIGILL 杀掉。 */
    uint64_t proc_fp_regs[32];
    uint64_t proc_fcsr;
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
/* 被信号杀死的退出路径：wait status 的低 7 位是信号号，而不是 (code<<8) */
void do_exit_signal(int sig) __attribute__((noreturn));
int16_t do_wait(int16_t pid, int *status, int options);
int do_exec(intstkf_t *sp, const char *path, char *const *argv, char *const *envp);
int16_t create_kernel_thread_by_fork(void *func(void *), void *args, uint32_t clone_flags);
/* 按 pid 查找 pcb（find_proc_by_pid 的公开包装）；未找到返回 NULL。
 * @note 内部自取 proc_list_lock，**调用者不得已经持有它**（自旋锁不可重入）。 */
pcb_t *proc_find_by_pid(int16_t pid);

/* 在 proc_list_lock 保护下按 pid / pgid 找到进程并**立刻**对它调用 fn。
 * kill 这类"查到就要动手"的场景必须用它，而不是先 proc_find_by_pid 再动手——
 * 后者在两步之间有目标被 do_wait 收割（list_del + kfree）的窗口。
 * 锁序：tty->lock → proc_list_lock → sighand->lock → run_queue.lock，单向；
 * 因此 fn 里可以发信号、可以 wakeup，但不得睡眠、不得取 VFS 大锁。 */
bool proc_apply_by_pid(int16_t pid, void (*fn)(pcb_t *p, int arg), int arg);
/* 返回被应用到的进程个数（0 表示该进程组不存在） */
int proc_apply_by_pgid(int16_t pgid, void (*fn)(pcb_t *p, int arg), int arg);
/* 进程子系统的全局结构初始化，hart0 专用且必须早于 cpu_start_secondary_hart()。 */
void proc_early_init(void);
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
