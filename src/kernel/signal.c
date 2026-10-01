/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "signal.h"
#include "proc.h"
#include "sched.h"
#include "slab.h"
#include "kmalloc.h"
#include "stringops.h"
#include "errorcode.h"
#include "console.h"
#include "vmm.h"
#include "pmm.h"
#include "memtype.h"
#include "syscall.h"
#include "uaccess.h"

/**
 * @brief 压在用户栈上的信号帧
 * @note 布局是内核自己定的：本项目不支持 SA_SIGINFO，用户 handler 拿不到 ucontext，
 *   所以没有必须与 Linux 逐字节对齐的理由。唯一的读者是 sys_rt_sigreturn()。
 */
typedef struct sigframe
{
    sigset_t saved_mask;   /* 进入 handler 之前的屏蔽字 */
    intstkf_t saved_regs;  /* 整个 trap 帧原样备份，含 sepc/sstatus */
} sigframe_t;

/**
 * @brief 每进程的信号处理表（signal.h 里只有前向声明，实体在这里）
 * @note lock 同时保护宿主 pcb 的 proc_sig_pending 与 proc_sig_mask，写必须持锁。
 *   pending 是真的跨 hart 共享（任何进程都能 kill 它），mask 实际只有任务自己写，
 *   但一并放进同一把锁下，免得留下"哪个字段要锁"的模糊地带。
 *   唯一的例外是 signal_pending() 的纯读，理由见它的声明处。
 */
struct sighand
{
    osslock_t lock;
    struct linux_sigaction actions[NSIG]; /* actions[0] 不用，下标即信号号 */
};

/* sighand_t 有 1500 多字节，用自己的命名 cache 而不是走 kmalloc 的尺寸类
 * （最大 2048，会浪费近 500 字节）。建在 signal_init 里而不是 slab_init 里，
 * 因为 sighand_t 对 slab.c 是不透明类型，那边取不到 sizeof。 */
static kmem_cache_t *sighand_cache;

/* sigpage 的物理帧：全系统只有一份，被所有用户地址空间共享 */
static pframe_t *sigpage_frame;

/* li a7, 139(__NR_rt_sigreturn) ; ecall
 * 手工编码，避免为两条指令单独开一个汇编文件：
 *   addi a7, x0, 139 → imm=0x08b, rs1=0, funct3=0, rd=17, opcode=0x13 → 0x08b00893
 *   ecall                                                            → 0x00000073 */
static const uint32_t sigreturn_code[2] = {0x08b00893, 0x00000073};

/**
 * @brief 各信号的默认动作（SIG_DFL 时生效）
 * @details 表填满 NSIG 项而不是只填 1..31，是为了让 actions[] 越界访问在编译期就
 *   不可能发生。core dump 类信号（SIGQUIT/SIGILL/SIGABRT/SIGSEGV 等）本项目没有
 *   地方写 core，一律按终止处理。
 */
static const uint8_t default_action[NSIG] = {
    [SIGCHLD] = SIG_ACT_IGNORE,
    [SIGURG]  = SIG_ACT_IGNORE,
    [SIGWINCH] = SIG_ACT_IGNORE,
    [SIGCONT] = SIG_ACT_IGNORE,
    /* 其余项由静态初始化补 0 == SIG_ACT_TERM */
};

/**
 * @brief 准备 sigpage 的物理页
 * @note 必须在 MMU 开启之后、proc_init() 之前由 hart0 调用一次。
 * @details 这一页永远不会被释放：分配后把 reference 额外 +1 作为永久基线，
 *   于是所有进程都退干净之后计数也只回到 1，vmm_unmap_range 的递减永远碰不到 0。
 *   看到"一页被 N 个进程引用且从不归还"不是泄漏，是有意为之。
 *   基线只顶住"最后一个进程退出"这一下——每建立一次映射照样要 reference++
 *   （在 vmm_map_fixed_page 里做），否则第一个进程退出就把这页还给 PMM 了。
 */
void signal_init(void)
{
    sighand_cache = slab_cache_create("sighand", sizeof(struct sighand));
    if (sighand_cache == NULL)
    {
        panic("signal_init: cannot create sighand cache");
    }

    sigpage_frame = pmm_alloc_pages(1);
    if (sigpage_frame == NULL)
    {
        panic("signal_init: cannot allocate sigpage");
    }
    void *kva = (void *)convert_pframe2kva(sigpage_frame);
    memset(kva, 0, PGSIZE);
    memcpy(kva, sigreturn_code, sizeof(sigreturn_code));

    sigpage_frame->reference += 1; /* 永久基线，见上 */
    printf("signal: sigpage ready\n");
}

/**
 * @brief 把 sigpage 映进一个用户地址空间
 * @details PTE 取 R|X（不带 W），并且立刻落实而不是建成懒分配的 VMA——
 *   懒分配会在缺页时分配一张全新的空白页，取指取到 0。
 *   VMA 仍然要建：vmm_mm_copy / vmm_mm_destroy 都按 VMA 链表干活，缺了它
 *   fork 出来的子进程就没有这页映射。
 */
int signal_map_sigpage(mm_t *mm)
{
    vma_t *vma = vmm_vma_create(USER_SIGPAGE, USER_SIGPAGE + PGSIZE, VMP_R | VMP_X);
    if (vma == NULL)
    {
        return ENO1_NOMORE_MEM;
    }
    vmm_vma_insert(mm, vma);
    return vmm_map_fixed_page(mm, USER_SIGPAGE,
                              convert_pframe2ppn(sigpage_frame), VMP_R | VMP_X);
}

/**
 * @brief 给一个进程分配 sighand_t（全部动作置 SIG_DFL）
 */
sighand_t *signal_hand_create(void)
{
    sighand_t *sh = slab_cache_alloc(sighand_cache);
    if (sh == NULL)
    {
        return NULL;
    }
    spinlock_init(&sh->lock);
    memset(sh->actions, 0, sizeof(sh->actions)); /* SIG_DFL == 0 */
    return sh;
}

/**
 * @brief fork 用：深拷贝父进程的 sighand_t
 */
sighand_t *signal_hand_copy(sighand_t *src)
{
    if (src == NULL)
    {
        return NULL;
    }
    sighand_t *dst = slab_cache_alloc(sighand_cache);
    if (dst == NULL)
    {
        return NULL;
    }
    spinlock_init(&dst->lock);

    irq_key_t key = spinlock_acquire(&src->lock);
    memcpy(dst->actions, src->actions, sizeof(dst->actions));
    spinlock_release(&src->lock, key);
    return dst;
}

/**
 * @brief execve 成功后重置信号处理表
 * @details POSIX：被捕获的（自定义 handler）一律回到 SIG_DFL——新程序的代码段已经
 *   换掉，旧 handler 地址指向的是别人的内存；SIG_IGN 与 SIG_DFL 原样保留；
 *   屏蔽字 proc_sig_mask 跨 exec 不变。
 */
void signal_hand_reset_on_exec(pcb_t *p)
{
    if (p->proc_sighand == NULL)
    {
        return;
    }
    irq_key_t key = spinlock_acquire(&p->proc_sighand->lock);
    for (int sig = 1; sig < NSIG; sig++)
    {
        struct linux_sigaction *act = &p->proc_sighand->actions[sig];
        if (act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN)
        {
            act->sa_handler = SIG_DFL;
            act->sa_flags = 0;
            act->sa_mask = 0;
        }
    }
    spinlock_release(&p->proc_sighand->lock, key);
}

/**
 * @brief 是否有未被屏蔽的挂起信号（SIGKILL 不受屏蔽字影响）
 */
bool signal_pending(pcb_t *p)
{
    if (p == NULL || p->proc_sighand == NULL)
    {
        return false;
    }
    sigset_t deliverable = (p->proc_sig_pending & ~p->proc_sig_mask) |
                           (p->proc_sig_pending & SIG_UNCATCHABLE);
    return deliverable != 0;
}

/**
 * @brief 产生阶段：置挂起位，必要时把目标从可中断睡眠里踢醒
 * @param[in] p   目标进程
 * @param[in] sig 信号号 1..NSIG-1；0 表示只做存在性检查（POSIX 的 kill -0 语义）
 * @retval ENO0_NO_ERROR 已投递（或按"默认忽略"直接丢弃）
 * @retval ENO16_PERM    目标是内核线程（proc_sighand == NULL），不接收信号
 * @retval ENO6_INVAL_PARAM 信号号非法
 * @note 可在中断上下文调用（^C 走的就是这条路）：全程只用自旋锁，不碰用户内存、
 *   不睡眠。真正的投递发生在目标自己返回 U 态那一刻，最迟一个 tick 之后，
 *   所以这里不需要为了"尽快投递"给别的 hart 发 IPI。
 */
int signal_send(pcb_t *p, int sig)
{
    if (p == NULL)
    {
        return ENO25_NO_SUCH_PROC;
    }
    if (sig < 0 || sig >= NSIG)
    {
        return ENO6_INVAL_PARAM;
    }
    /* 内核线程不接收信号：它没有返回 U 态的时刻，投递点永远不会被执行，
     * 置了位也只是让 signal_pending() 恒真而已 */
    if (p->proc_sighand == NULL)
    {
        return ENO16_PERM;
    }
    if (sig == 0)
    {
        return ENO0_NO_ERROR; /* 只做存在性检查 */
    }
    if (p->proc_state == ZOMBIE)
    {
        return ENO0_NO_ERROR; /* 已经死了，丢弃 */
    }

    irq_key_t key = spinlock_acquire(&p->proc_sighand->lock);

    /* 投递前的忽略优化：动作是忽略的信号连位都不置。这样 SIGCHLD（默认忽略）
     * 在每次子进程退出时不会白白让父进程的 signal_pending() 变真 */
    if ((sigmask(sig) & SIG_UNCATCHABLE) == 0)
    {
        uint64_t handler = p->proc_sighand->actions[sig].sa_handler;
        if (handler == SIG_IGN ||
            (handler == SIG_DFL && default_action[sig] == SIG_ACT_IGNORE))
        {
            spinlock_release(&p->proc_sighand->lock, key);
            return ENO0_NO_ERROR;
        }
    }

    p->proc_sig_pending |= sigmask(sig);
    spinlock_release(&p->proc_sighand->lock, key);

    /* 放锁之后再唤醒：wakeup 要取就绪队列锁，目标一旦跑起来又会来抢这把
     * sighand->lock，先放掉能省掉一次无谓的自旋 */
    if (p->proc_state == INTERRUPTIBLE)
    {
        wakeup(p);
    }
    return ENO0_NO_ERROR;
}

/* 取集合里编号最小的信号，空集返回 0。
 * 不用 __builtin_ctzl：rv64 没有 Zbb 的 ctz 指令，gcc 会展开成 libgcc 的
 * __ctzdi2，而这里是 -nostdlib 的freestanding 环境，链接期直接找不到符号。
 * 投递路径每次 trap 最多走几次，逐位扫的常数完全无所谓。 */
static int sigset_first(sigset_t set)
{
    for (int i = 0; i < NSIG; i++)
    {
        if (set & (1UL << i))
        {
            return i + 1;
        }
    }
    return 0;
}

/**
 * @brief 在用户栈上构造信号帧并把 trap 帧改写成"进入 handler"
 * @retval ENO0_NO_ERROR 成功
 * @retval ENO8_NULL_POINTER 用户栈写不进去（sp 被改坏 / 越界）
 * @note 这里会往用户栈写，很可能触发懒分配缺页——所以不能持有任何自旋锁。
 */
static int signal_setup_frame(intstkf_t *sp, int sig, const struct linux_sigaction *act)
{
    pcb_t *cur = proc_get_current();

    /* RISC-V ABI 要求进入函数时 sp 16 字节对齐；该架构没有 red zone，不用额外留空 */
    virAddr_t frame_va = (sp->x2_sp - sizeof(sigframe_t)) & ~15UL;

    sigframe_t frame;
    frame.saved_mask = cur->proc_sig_mask;
    frame.saved_regs = *sp;
    if (copy_to_user((void *)frame_va, &frame, sizeof(frame)) != 0)
    {
        return ENO8_NULL_POINTER;
    }

    sp->sepc = act->sa_handler;
    sp->x2_sp = frame_va;
    sp->x10_a0 = (uint64_t)sig;   /* handler 的第一个参数 */
    sp->x1_ra = USER_SIGPAGE;     /* handler ret 过去就是一条 rt_sigreturn */

    /* POSIX：handler 执行期间自动屏蔽本信号（除非 SA_NODEFER）。漏掉这一步的话
     * handler 里再收到同一个信号会无限递归压帧，直到把用户栈压爆。
     * 改屏蔽字与 SA_RESETHAND 合到同一个临界区里——用户栈已经写完，这里不会再缺页。 */
    sigset_t block = act->sa_mask;
    if ((act->sa_flags & SA_NODEFER) == 0)
    {
        block |= sigmask(sig);
    }

    irq_key_t key = spinlock_acquire(&cur->proc_sighand->lock);
    cur->proc_sig_mask |= (block & ~SIG_UNCATCHABLE);
    if (act->sa_flags & SA_RESETHAND)
    {
        cur->proc_sighand->actions[sig].sa_handler = SIG_DFL;
        cur->proc_sighand->actions[sig].sa_flags = 0;
        cur->proc_sighand->actions[sig].sa_mask = 0;
    }
    spinlock_release(&cur->proc_sighand->lock, key);
    return ENO0_NO_ERROR;
}

/**
 * @brief 投递阶段：把挂起的信号在"马上要 sret 回 U 态"这一刻兑现
 * @param[in,out] sp 当前 trap 帧；投递用户 handler 时会被改写
 * @details 只由 trap_handler() 在 sstatus.SPP == 0 时调用。流程：
 *   1. 取出编号最小的一个可投递信号（pending & ~mask，SIGKILL 不受 mask 约束）；
 *   2. 按动作分派：忽略则丢弃后继续取下一个；终止则 do_exit_signal 不返回；
 *      自定义 handler 则先结算 -ERESTARTSYS，再压帧并立刻返回（一次 trap 只投
 *      一个 handler，其余留在挂起集里，等 rt_sigreturn 那次 ecall 再走一遍这里）；
 *   3. 没有可投递信号时只负责结算 -ERESTARTSYS（无条件重启被打断的 syscall）。
 * @note 全程不得持有任何自旋锁：往用户栈压帧会触发懒分配缺页，缺页处理要取
 *   vmm_lock，还可能走到 slab 的 reclaim。读写 sighand 只在取值的瞬间加锁。
 */
void signal_handle_pending(intstkf_t *sp)
{
    pcb_t *cur = proc_get_current();
    if (cur == NULL || cur->proc_sighand == NULL)
    {
        return;
    }

    /* 只有"这次 trap 是用户 ecall"时 a0 才是 syscall 返回值。时钟中断/缺页也会走到
     * 投递点，那时 a0 只是用户的一个普通寄存器，可能恰好装着 -512——不判 scause 就会
     * 把它误当成 ERESTARTSYS，回退 sepc 去重执行一条根本不是 ecall 的指令。 */
    bool in_syscall = ((sp->scause & (1UL << 63)) == 0) &&
                      ((sp->scause & CAUSE_SUPERVISOR_IRQ_REASON_MASK) == CAUSE_USER_ECALL);
    bool restart_wanted = in_syscall && ((long)sp->x10_a0 == ENO24_RESTARTSYS);

    while (1)
    {
        /* 取信号、清挂起位、读动作三步必须在同一个临界区里完成。
         * 清位是读-改-写，而另一个 hart 的 signal_send 正在锁里 `|=` 新的位——
         * 在锁外做的话，那个刚置上的位会被这次回写整个抹掉，等于凭空吞掉一个信号。 */
        struct linux_sigaction act;
        int sig;
        irq_key_t key = spinlock_acquire(&cur->proc_sighand->lock);
        sigset_t deliverable = (cur->proc_sig_pending & ~cur->proc_sig_mask) |
                               (cur->proc_sig_pending & SIG_UNCATCHABLE);
        if (deliverable == 0)
        {
            spinlock_release(&cur->proc_sighand->lock, key);
            break;
        }
        sig = sigset_first(deliverable);
        cur->proc_sig_pending &= ~sigmask(sig);
        act = cur->proc_sighand->actions[sig];
        spinlock_release(&cur->proc_sighand->lock, key);

        if ((sigmask(sig) & SIG_UNCATCHABLE) != 0)
        {
            act.sa_handler = SIG_DFL; /* SIGKILL/SIGSTOP 永远走默认动作 */
        }

        if (act.sa_handler == SIG_IGN)
        {
            continue;
        }
        if (act.sa_handler == SIG_DFL)
        {
            if (default_action[sig] == SIG_ACT_IGNORE)
            {
                continue;
            }
            do_exit_signal(sig); /* 不返回 */
        }

        /* 顺序不能反：先把被打断的 syscall 结算掉（改 sepc/a0），再压帧。
         * 反过来写的话，帧里存的是"还没结算"的现场，handler 返回后会跳到 ecall
         * 后面一条指令、a0 里躺着一个 -512 */
        if (restart_wanted)
        {
            if (act.sa_flags & SA_RESTART)
            {
                sp->sepc -= 4; /* 退回到那条 ecall，重新执行 */
                sp->x10_a0 = cur->proc_syscall_orig_a0;
            }
            else
            {
                sp->x10_a0 = (uint64_t)(long)ENO26_INTERRUPTED;
            }
            restart_wanted = false;
        }

        if (signal_setup_frame(sp, sig, &act) != ENO0_NO_ERROR)
        {
            /* 用户栈已经不可写（多半是 sp 被程序自己改坏了），除了杀掉它没别的选择 */
            printf("signal: cannot push frame for sig=%d, killing pid=%d\n",
                   sig, cur->proc_pid);
            do_exit_signal(SIGSEGV);
        }
        return; /* 一次 trap 只投一个 handler */
    }

    if (restart_wanted)
    {
        /* 没有信号要投递（被打断的原因已经消失，比如信号被同时屏蔽掉了）：
         * 无条件重启，用户程序完全看不到这次打断 */
        sp->sepc -= 4;
        sp->x10_a0 = cur->proc_syscall_orig_a0;
    }
}

/**
 * @brief handler 执行完、经 sigpage 蹦床回来时恢复现场
 * @param[in,out] sp 当前 trap 帧，将被帧里备份的内容整体覆盖
 * @return 被信号打断的那次 syscall 的返回值（见下面关于 a0 的说明）
 */
long signal_do_sigreturn(intstkf_t *sp)
{
    pcb_t *cur = proc_get_current();
    sigframe_t frame;

    if (copy_from_user(&frame, (void *)sp->x2_sp, sizeof(frame)) != 0)
    {
        printf("signal: bad sigreturn frame at 0x%lx, killing pid=%d\n",
               sp->x2_sp, cur->proc_pid);
        do_exit_signal(SIGSEGV);
    }

    irq_key_t key = spinlock_acquire(&cur->proc_sighand->lock);
    cur->proc_sig_mask = frame.saved_mask & ~SIG_UNCATCHABLE;
    spinlock_release(&cur->proc_sighand->lock, key);

    *sp = frame.saved_regs;

    /* sstatus 只恢复固定组合，不能整个信任用户帧——用户完全可以把帧里的 SPP
     * 改成 1，那就是一张回 S 态的直通车票。写法与 do_exec 构造 trap 帧时一致 */
    sp->sstatus = (read_csr(sstatus) & ~SSTATUS_SPP) | SSTATUS_SPIE;

    /* 必须把恢复出来的 a0 当返回值交回去：trap_dispatch 那句
     * `sp->x10_a0 = syscall_dispatch(sp)` 会无条件覆盖 a0，返回别的值就等于
     * 把被打断的那次 syscall 的返回值改成垃圾。返回它自己 → 那句赋值变成幂等 */
    return (long)sp->x10_a0;
}

/**
 * @brief rt_sigaction 的内核侧实现（用户指针的进出由调用者负责）
 * @param[in]  sig  信号号
 * @param[in]  act  新动作，NULL 表示只查询
 * @param[out] oact 出参：原动作，NULL 表示不关心
 */
int signal_action_set(int sig, const struct linux_sigaction *act,
                      struct linux_sigaction *oact)
{
    pcb_t *cur = proc_get_current();
    if (!signal_valid(sig) || cur->proc_sighand == NULL)
    {
        return ENO6_INVAL_PARAM;
    }
    /* SIGKILL / SIGSTOP 不可捕获也不可忽略 */
    if (act != NULL && (sigmask(sig) & SIG_UNCATCHABLE) != 0)
    {
        return ENO6_INVAL_PARAM;
    }
    /* SA_SIGINFO 的 handler 会去读第二、三个参数（siginfo_t / ucontext），
     * 两者都不构造，显式拒绝比默默给它两个未初始化的寄存器安全 */
    if (act != NULL && (act->sa_flags & SA_SIGINFO))
    {
        return ENO6_INVAL_PARAM;
    }

    irq_key_t key = spinlock_acquire(&cur->proc_sighand->lock);
    if (oact != NULL)
    {
        *oact = cur->proc_sighand->actions[sig]; /* 先出后进：用户可能两个参数传同一个指针 */
    }
    if (act != NULL)
    {
        cur->proc_sighand->actions[sig] = *act;
    }
    spinlock_release(&cur->proc_sighand->lock, key);
    return ENO0_NO_ERROR;
}

/**
 * @brief rt_sigprocmask 的内核侧实现（只认内核指针）
 */
int signal_mask_set(int how, const sigset_t *set, sigset_t *oset)
{
    pcb_t *cur = proc_get_current();
    if (cur->proc_sighand == NULL)
    {
        return ENO6_INVAL_PARAM;
    }

    irq_key_t key = spinlock_acquire(&cur->proc_sighand->lock);
    if (oset != NULL)
    {
        *oset = cur->proc_sig_mask;
    }
    if (set != NULL)
    {
        switch (how)
        {
        case SIG_BLOCK:
            cur->proc_sig_mask |= *set;
            break;
        case SIG_UNBLOCK:
            cur->proc_sig_mask &= ~(*set);
            break;
        case SIG_SETMASK:
            cur->proc_sig_mask = *set;
            break;
        default:
            spinlock_release(&cur->proc_sighand->lock, key);
            return ENO6_INVAL_PARAM;
        }
        /* SIGKILL / SIGSTOP 屏蔽不掉，落盘前强行清掉这两位 */
        cur->proc_sig_mask &= ~SIG_UNCATCHABLE;
    }
    spinlock_release(&cur->proc_sighand->lock, key);
    return ENO0_NO_ERROR;
}

/**
 * @brief 当前进程的挂起集（rt_sigpending）
 */
sigset_t signal_pending_set(void)
{
    return proc_get_current()->proc_sig_pending;
}

/* proc_apply_by_pid / proc_apply_by_pgid 的回调：arg 就是信号号。
 * 调用时 proc_list_lock 在手，所以这里只能做 signal_send 这类不睡眠的事。 */
static void send_one(pcb_t *p, int sig)
{
    signal_send(p, sig);
}

/**
 * @brief 按 pid 发信号；查找与投递在 proc_list_lock 里一气呵成
 */
int signal_send_pid(int16_t pid, int sig)
{
    if (!proc_apply_by_pid(pid, send_one, sig))
    {
        return ENO25_NO_SUCH_PROC;
    }
    return ENO0_NO_ERROR;
}

/**
 * @brief 给整个进程组发信号
 */
int signal_send_group(int16_t pgid, int sig)
{
    if (pgid <= 0)
    {
        return ENO6_INVAL_PARAM;
    }
    if (proc_apply_by_pgid(pgid, send_one, sig) == 0)
    {
        return ENO25_NO_SUCH_PROC;
    }
    return ENO0_NO_ERROR;
}
