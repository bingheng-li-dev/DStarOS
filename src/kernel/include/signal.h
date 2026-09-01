#ifndef _SIGNAL_H_
#define _SIGNAL_H_

#include <stdint.h>
#include <stdbool.h>

#include "trap.h"

struct proc_control_block;
struct mm_struct;

/* ============================================================
 * 信号编号（Linux asm-generic，riscv64 用的就是这一套）
 * ============================================================ */
#define SIGHUP     1
#define SIGINT     2
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGBUS     7
#define SIGFPE     8
#define SIGKILL    9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

/* 信号号上限：sigset_t 就是 64 位，1..31 是标准信号，32.. 是实时信号（本阶段不投递） */
#define NSIG 64

/* ============================================================
 * sigaction / sigprocmask 的常量
 * ============================================================ */
#define SIG_DFL 0UL
#define SIG_IGN 1UL

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_NOCLDSTOP 0x00000001UL
#define SA_NOCLDWAIT 0x00000002UL
#define SA_SIGINFO   0x00000004UL
#define SA_ONSTACK   0x08000000UL
#define SA_RESTART   0x10000000UL
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL

typedef uint64_t sigset_t;

/**
 * @brief 内核 ABI 的 struct sigaction（riscv64 未定义 SA_RESTORER，所以没有那个字段）
 * @details sa_mask 放在最后是 Linux 内核有意为之（"mask last for extensibility"），
 *   **不要按 libc 用户态那个 struct sigaction 的顺序写**——那个 sa_mask 在中间，
 *   布局对不上会把 handler 装成垃圾指针。
 */
struct linux_sigaction
{
    uint64_t sa_handler; /* 偏移 0：函数指针，或 SIG_DFL(0) / SIG_IGN(1) */
    uint64_t sa_flags;   /* 偏移 8 */
    sigset_t sa_mask;    /* 偏移 16 */
};

_Static_assert(sizeof(struct linux_sigaction) == 24,
               "linux_sigaction size must match Linux kernel ABI");

/* rt_sigaction / rt_sigprocmask / rt_sigpending 的 sigsetsize 参数只接受这个值 */
#define SIGSET_SIZE 8

/**
 * @brief 每进程的信号处理表。**对外不透明**，定义在 signal.c 里。
 * @details 独立成对象、不内嵌进 pcb_t——actions[] 有 1536 字节，内嵌会让每个内核
 *   线程的 PCB 也一起变胖好几倍。不透明是为了让本头文件不必 include sync.h：
 *   sync.h → cpu.h → proc.h → signal.h 是一个环，signal.h 一旦 include sync.h，
 *   从 console.h 这条路径进来时就会拿到一个空的 sync.h（头文件卫兵已置位）。
 */
typedef struct sighand sighand_t;

/* 默认动作 */
#define SIG_ACT_TERM   0 /* 终止进程（core 也按终止处理，不做 core dump） */
#define SIG_ACT_IGNORE 1 /* 忽略 */

/* 信号号 n 对应的位掩码：信号号从 1 起、bit 从 0 起，差一是这里最常见的笔误 */
static inline sigset_t sigmask(int sig)
{
    return 1UL << (sig - 1);
}

/* 不可捕获、不可屏蔽的信号（本阶段不做 stop，SIGSTOP 只是拒绝装 handler） */
#define SIG_UNCATCHABLE (sigmask(SIGKILL) | sigmask(SIGSTOP))

static inline bool signal_valid(int sig)
{
    return sig > 0 && sig < NSIG;
}

/* sigpage：一页只读可执行的内核页，里面是 `li a7,__NR_rt_sigreturn; ecall`。
 * 用户栈是 VMP_R|VMP_W、不可执行，蹦床没法放栈上，只能单独给一页。
 * 地址取用户栈底（USER_STACK_TOP - USER_STACK_LEN = 0x3fff0000）之下一页。 */
#define USER_SIGPAGE 0x3ffef000UL

/* 初始化 sigpage 的物理页；须在 MMU 之后、proc_init 之前调用一次。 */
void signal_init(void);
/* 把 sigpage 映进一个用户地址空间；由 create_user_mm() 调用。 */
int signal_map_sigpage(struct mm_struct *mm);

/* 给一个进程分配 sighand_t（全部动作置 SIG_DFL）；用户进程才需要。 */
sighand_t *signal_hand_create(void);
/* fork 用：深拷贝父进程的 sighand_t。 */
sighand_t *signal_hand_copy(sighand_t *src);
/* exec 用：被捕获的动作重置为 SIG_DFL，SIG_IGN / SIG_DFL 原样保留。 */
void signal_hand_reset_on_exec(struct proc_control_block *p);

/* 产生阶段：置位 + 必要时唤醒。可在中断上下文调用，不碰用户内存，不睡眠。 */
int signal_send(struct proc_control_block *p, int sig);
/* 按 pid 发；查找与投递在 proc_list_lock 里一气呵成，没有"查到之后目标被收割"的窗口。 */
int signal_send_pid(int16_t pid, int sig);
/* 给整个进程组发；同上。 */
int signal_send_group(int16_t pgid, int sig);

/**
 * @brief 是否有未被屏蔽的挂起信号（SIGKILL 不受屏蔽字影响）
 * @note **有意不取 sighand 锁**：这是一次纯读，rv64 上 64 位对齐读本身是原子的，
 *   最坏情况读到略旧的值、多睡一轮而已。更重要的是它的调用点都在管道/TTY 的
 *   条件锁里面，取锁会引入 pipe->lock → sighand->lock 这条依赖，没必要。
 */
bool signal_pending(struct proc_control_block *p);

/* 投递阶段：只在"带着当前任务返回 U 态"之前调用（trap_handler 尾部）。 */
void signal_handle_pending(intstkf_t *sp);

/* 以下三个是 syscall 壳的内核侧实现，只认内核指针；用户指针的进出由 syscall.c 负责。 */
int signal_action_set(int sig, const struct linux_sigaction *act,
                      struct linux_sigaction *oact);
int signal_mask_set(int how, const sigset_t *set, sigset_t *oset);
sigset_t signal_pending_set(void);
/* handler 返回、经 sigpage 蹦床进来时恢复现场；返回值即被打断的 syscall 的返回值。 */
long signal_do_sigreturn(intstkf_t *sp);

#endif /* _SIGNAL_H_ */
