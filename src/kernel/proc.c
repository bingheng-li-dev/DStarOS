/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "proc.h"
#include "slab.h"
#include "kmalloc.h"
#include "stringops.h"
#include "errorcode.h"
#include "sched.h"
#include "console.h"
#include "vfs.h"
#include "tty.h"
#include "cpu.h"
#include "vmm.h"
#include "elf.h"
#include "sbi.h"
#include "uaccess.h"
#include "linux_abi.h"
#include "ktime.h"
#include "fpu.h"
#include "suites.h"

/* 全部进程的链表（各 hart 的 idle 不在其中） */
struct list_head proc_list;
/* 保护 proc_list，以及进程父子链（proc_children / proc_sibling_linker / proc_parent）。
 * sys_kill 遍历找目标、孤儿过继给 init，都会与 do_wait 收割（摘链 + kfree）并发。
 * 锁序：tty->lock → proc_list_lock → sighand->lock → run_queue.lock，单向；
 * 持有本锁期间不得睡眠、不得取 VFS 大锁。 */
static osslock_t proc_list_lock;
/* pid 位图：第 n 位为 1 表示 pid n 已被占用（ZOMBIE 也算，收割时才释放）。
 * 用独立的锁，不与 proc_list_lock 嵌套。 */
static osslock_t pid_lock;
static uint64_t pid_bitmap[PID_MAX_VALUE / 64 + 1];
/* 进程数（含各 hart 的 idle） */
uint16_t task_count = 0;

#define USER_STACK_LEN  (16 * PGSIZE)      /* 64 KB，懒分配 */

#define EXEC_MAX_ARGS       64          /* argc + envc 的合计上限 */
#define EXEC_ARG_STRLEN_MAX 256         /* 单个参数串的长度上限（含结束符） */
#define EXEC_ARG_BUF_SIZE   PGSIZE      /* 字符串区总字节上限 */

/**
 * @brief execve 参数在内核侧的暂存区
 * @details argv/envp 是二级指针：先要读指针数组、再逐个跟着指针读字符串，
 *   两级都在旧地址空间里，所以必须赶在切 satp 之前全部拷进内核。
 *   这里只存紧凑排列的字符串与它们的偏移，不存指针——暂存区里的地址与最终
 *   要写进用户栈的地址毫无关系。
 */
typedef struct exec_args
{
    int      argc;
    int      envc;
    int      n;                      /* 已收进的串总数，恒等于 argc + envc */
    char    *buf;                    /* 字符串区，argc 个串在前、envc 个在后 */
    uint32_t used;                   /* buf 已用字节 */
    uint32_t off[EXEC_MAX_ARGS];     /* 每个串在 buf 中的起始偏移 */
} exec_args_t;

static pcb_t *alloc_new_proc(void);
static int16_t alloc_kernel_stack(pcb_t *pcb);
static int16_t dealloc_kernel_stack(pcb_t *pcb);
static int16_t copy_proc_mm(uint32_t clone_flags, pcb_t *pcb);
static void copy_proc_stk(pcb_t *pcb, uintptr_t stack, intstkf_t *regs);
static pcb_t *create_first_proc_idle(void);
static mm_t *create_user_mm(void);
static pcb_t *find_proc_by_pid(int16_t pid);
static int16_t alloc_pid_map(void);
static void dealloc_pid_map(int16_t pid);
static int16_t init(void);
static void fork_out(void);
#if DEBUG_PROC_CTXSTK
static void print_ctx_stk(ctx_t *ctx) __attribute__((used));
#endif

/**
 * @brief 设置进程名，超长截断
 */
char *set_proc_name(pcb_t *proc, const char *name)
{
    size_t n = strlen(name);
    if (n > PNAME_MAX_LENGTH)
    {
        n = PNAME_MAX_LENGTH;
    }
    memset(proc->proc_pname, 0, sizeof(proc->proc_pname));
    return memcpy(proc->proc_pname, name, n);
}

/**
 * @brief 复制当前进程
 * @param[in] stack 子进程的用户栈指针；0 表示 fork 内核线程
 * @return 子进程 pid；负值为错误码
 */
int do_fork(uint32_t clone_flags, uintptr_t stack, intstkf_t *regs)
{
    if (task_count > PROC_MAX_AMOUNT)
    {
        goto f1;
    }
    pcb_t *new_proc = alloc_new_proc();
    if (new_proc == NULL)
    {
        goto f1;
    }
    int16_t ret = ENO0_NO_ERROR;
    ret = alloc_kernel_stack(new_proc);
    if (ret == ENO1_NOMORE_MEM)
    {
        goto f2;
    }

    /* 信号：继承屏蔽字与全部 handler，但 挂起集清零——父进程还没处理完的信号
     * 不该让子进程再收一遍（POSIX）。父进程是内核线程（proc_sighand == NULL）时
     * 子进程也是内核线程，什么都不用做。
     * 放在 copy_proc_mm 之前是为了让失败路径只需要还内核栈与 PCB——一旦建了新
     * 地址空间再失败，回滚要复杂得多。 */
    new_proc->proc_pgid = proc_get_current()->proc_pgid;
    if (proc_get_current()->proc_sighand != NULL)
    {
        new_proc->proc_sighand = signal_hand_copy(proc_get_current()->proc_sighand);
        if (new_proc->proc_sighand == NULL)
        {
            goto f3;
        }
        new_proc->proc_sig_mask = proc_get_current()->proc_sig_mask;
        new_proc->proc_sig_pending = 0;
    }

    /* pid 也在 copy_proc_mm 之前分配，理由同上 */
    int16_t pid = alloc_pid_map();
    if (pid < 0)
    {
        goto f4;
    }
    new_proc->proc_pid = pid;

    copy_proc_mm(clone_flags, new_proc);
    copy_proc_stk(new_proc, stack, regs);

    /* 浮点上下文按 fork 的语义整份继承：子进程从 fork 返回那一刻起，看到的寄存器
     * 必须和父进程一模一样。此刻父进程正跑在内核里、它的 f 寄存器还活在硬件上，
     * 所以要先存一次再拷——只拷 PCB 里那份的话，拿到的是父进程上一次被换出时
     * 的旧值。 */
    fpu_save(proc_get_current());
    memcpy(new_proc->proc_fp_regs, proc_get_current()->proc_fp_regs,
           sizeof(new_proc->proc_fp_regs));
    new_proc->proc_fcsr = proc_get_current()->proc_fcsr;

    /* umask 由 fork 继承（ITIMER_REAL 相反，POSIX 要求不继承，alloc_new_proc 已清零） */
    new_proc->proc_umask = proc_get_current()->proc_umask;

    new_proc->proc_cwd = proc_get_current()->proc_cwd;
    if (new_proc->proc_cwd)
    {
        /* d_ref 的增减一律在大锁内：别的 hart 可能正对同一个目录项 dentry_put。
         * 只在真有 cwd 时才取锁，理由同 do_exit 归还 cwd 那处。 */
        vfs_lock();
        vfs_dentry_get(new_proc->proc_cwd);
        vfs_unlock();
    }

    /* 子进程继承父进程的 fd 表：浅拷贝指针 + 每个 file_t 的 f_count++（父子共享打开文件与偏移）*/
    proc_fd_copy(new_proc, proc_get_current());

    /* 父子链与 proc_parent 一律在 proc_list_lock 下改：退出的进程会把孤儿过继给
     * init，与 init 自己的 do_wait 在不同 hart 上并发操作同一条 proc_children */
    irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
    list_add(&(new_proc->proc_list_linker), &(proc_list));
    task_count = task_count + 1;
    new_proc->proc_parent = proc_get_current();
    list_add_tail(&(new_proc->proc_sibling_linker), &(proc_get_current()->proc_children));
    spinlock_release(&proc_list_lock, plist_key);

    new_proc->proc_state = RUNNING;
    sched_activate(new_proc);

    return pid;
f1:
    return ENO2_ALLOCPROC_FAILED;
f2:
    kfree(new_proc);
    return ENO1_NOMORE_MEM;
f4:
    if (new_proc->proc_sighand != NULL)
    {
        kfree(new_proc->proc_sighand);
    }
    kfree((void *)(new_proc->kernel_stack));
    kfree(new_proc);
    return ENO3_NOFREE_PID;
f3:
    kfree((void *)(new_proc->kernel_stack));
    kfree(new_proc);
    return ENO1_NOMORE_MEM;
}

/**
 * @brief do_exit / do_exit_signal 共用的退出流程
 * @param[in] error_code 正常退出时的退出码（被信号杀死时无意义，传 0）
 * @param[in] sig        非 0 表示被该信号杀死，do_wait 据此编码 wait status
 * @note noreturn：末尾 sched_schedule() 换走之后再也不会被换回来。
 */
static void exit_common(int16_t error_code, uint8_t sig) __attribute__((noreturn));

/**
 * @brief 以退出码 error_code 结束当前进程
 */
void do_exit(int16_t error_code)
{
    exit_common(error_code, 0);
}

/**
 * @brief 当前进程被信号 sig 杀死
 */
void do_exit_signal(int sig)
{
    exit_common(0, (uint8_t)sig);
}

static void exit_common(int16_t error_code, uint8_t sig)
{
    pcb_t *curr = proc_get_current();

    /* PID 1 退出 = 系统失去 init：孤儿从此无人收割、shell 也再起不来，
     * 与其带着这个空洞继续跑，不如就地 panic（与 Linux 一致，也好定位）。
     * 必须放在最前面——下面的孤儿过继要 find_proc_by_pid(1)，那正是自己。
     * 回归形态下 PID 1 是内核线程、永远走不到这里，这条判断只对生产形态生效。 */
    if (curr->proc_pid == 1)
    {
        panic("init (pid 1) exited: code=%d sig=%d", (int)error_code, (int)sig);
    }

    /* 摘掉两条定时链上可能残留的节点。当前走不到（进程只能在自己的 nanosleep
     * 返回点之后才可能退出，那里已经摘过），但把 pcb 还给 slab 之前确保它不再
     * 挂在任何全局链表上是零成本的保险——真出现残留就是 tick 中断里的悬空指针。 */
    sched_timer_remove(curr);
    ktime_alarm_cancel(curr);

    proc_fd_close_all(curr);

    /* 归还工作目录的引用。只在真的 chdir 过时才取大锁，理由同 proc_fd_close_all()：
     * 内核线程的 proc_cwd 绝大多数是 NULL，不值得为此在退出路径上白添一个睡眠点。
     * 必须赶在置 ZOMBIE 之前做完——父进程一被唤醒就可能把这个 pcb 收割掉。 */
    if (curr->proc_cwd)
    {
        dentry_t *cwd = curr->proc_cwd;
        curr->proc_cwd = NULL;
        vfs_lock();
        vfs_dentry_put(cwd);
        vfs_unlock();
    }

    /* 孤儿过继给 init（pid 恒为 1：系统里第一个 do_fork 出来的进程）；
     * 有孤儿就唤醒 init，让它有机会发现并收割这些可能已经是 ZOMBIE 的孤儿 */
    pcb_t *init_proc = find_proc_by_pid(1);
    bool has_orphan = false;
    struct list_head *pos, *tmp;
    irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
    list_for_each_safe(pos, tmp, &curr->proc_children)
    {
        pcb_t *child = list_entry(pos, pcb_t, proc_sibling_linker);
        child->proc_parent = init_proc;
        list_del(&child->proc_sibling_linker);
        list_add_tail(&child->proc_sibling_linker, &init_proc->proc_children);
        has_orphan = true;
    }
    spinlock_release(&proc_list_lock, plist_key);
    if (has_orphan)
    {
        wakeup(init_proc);
    }

    if (curr->proc_mm)
    {
        /* 必须先切回内核页表再销毁 mm——现在站在的正是这张页表，vmm_mm_destroy
         * 释放的物理页一旦被 PMM 重新分配、内容被覆写，下一条指令取指/访存就会三重错误 */
        write_csr(satp, SATPMODE_RV39 | vmm_kernel_pgd_ppn);
        tlb_flush_all();
        vmm_mm_destroy(curr->proc_mm);
        curr->proc_mm = NULL;
    }
    curr->proc_exit_code = error_code;
    curr->proc_exit_sig = sig;
    /* pid 不在这里回收——ZOMBIE 期间 pid 必须继续"占用"，
     * 否则两次退出之间创建的新进程可能撞上同一个 pid。
     * 真正的回收在 do_wait() 收割时才做 */

    /* 置 ZOMBIE、读父进程、通知父进程都在 proc_list_lock 下：父进程只能在这把锁下
     * 被它自己的父进程收割，锁在手里就不会通知到一个已释放的 pcb；而父进程若已先
     * 退出，它在过继时（同一把锁下）早把 proc_parent 改成了 init。 */
    plist_key = spinlock_acquire(&proc_list_lock);
    curr->proc_state = ZOMBIE;
    pcb_t *parent = curr->proc_parent;
    if (parent)
    {
        /* SIGCHLD 默认动作是忽略，signal_send 的忽略优化会把它直接丢掉，
         * 所以对没装 handler 的父进程这一句等于零成本 */
        signal_send(parent, SIGCHLD);
        wakeup(parent);
    }
    spinlock_release(&proc_list_lock, plist_key);

    sched_schedule();

    panic("Zombie task resumed, should never happen");
}

/**
 * @brief 收割一个已退出的子进程
 * @param[in]  pid     -1 = 任意子进程；>0 = 只等这一个；0 / <-1（按进程组）未实现
 * @param[out] status  POSIX 编码的退出状态；NULL 表示不关心
 * @param[in]  options WNOHANG 生效；WUNTRACED / WCONTINUED 被忽略
 * @retval >0  被收割子进程的 pid
 * @retval 0   仅 WNOHANG：有匹配的子进程但都还活着
 * @retval <0  ENO*（ENO17_NO_CHILD：没有匹配的子进程；ENO20_NOSYS：按进程组等待）
 * @details
 *   `pid > 0` 这一档是 ash 的刚需：它按 pid 跟踪作业，收错一个就是 `$?` 错、
 *   或者一个已经死掉的作业永远等不到。`WNOHANG` 同理——ash 每次打提示符之前都会做
 *   一次非阻塞收割，忽略这个标志会让整个 shell 睡死在提示符之前。
 * @note WUNTRACED / WCONTINUED 恒被忽略：本内核没有 STOPPED 状态
 *   （`signal.h` 的 `SIG_UNCATCHABLE` 注释写明 SIGSTOP 只是拒绝装 handler），
 *   没有"停住的子进程"可报，所以忽略是当前语义下唯一诚实的做法。
 * @note 按进程组等待（`pid == 0` / `pid < -1`）返回 ENO20_NOSYS 而不是退化成
 *   "等任意"：ash 关掉 job control 之后不该走到这里，走到了就说明配置没关净，
 *   这个信号比一个含糊的实现有价值。
 */
int do_wait(int16_t pid, int *status, int options)
{
    pcb_t *cur = proc_get_current();

    if (pid == 0 || pid < -1)
    {
        return ENO20_NOSYS;
    }

    while (1)
    {
        /* 先置睡眠状态，再检查条件：若子进程恰好在这两步之间 do_exit()，
         * 它的 wakeup(cur) 会把状态改回 RUNNING，本轮循环末尾的 sched_schedule()
         * 会因为 curr 仍是 RUNNING 而重新入队、立刻continue，不会睡死过去 */
        cur->proc_state = INTERRUPTIBLE;

        bool has_child = false;
        pcb_t *child = NULL;
        struct list_head *pos;
        irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
        list_for_each(pos, &cur->proc_children)
        {
            pcb_t *c = list_entry(pos, pcb_t, proc_sibling_linker);
            if (pid > 0 && c->proc_pid != pid)
            {
                continue;
            }
            has_child = true;
            if (c->proc_state == ZOMBIE)
            {
                /* 在锁内摘链：之后既不会被 kill 查到，也不会被收割第二次 */
                list_del(&c->proc_sibling_linker);
                list_del(&c->proc_list_linker);
                task_count -= 1;
                child = c;
                break;
            }
        }
        spinlock_release(&proc_list_lock, plist_key);

        if (child != NULL)
        {
            cur->proc_state = RUNNING;

            /* 子进程置 ZOMBIE、唤醒父进程之后才 sched_schedule()，父进程醒来时它可能
             * 还站在自己的内核栈上。必须等 proc_on_cpu 清零（switch_to 已写完上下文），
             * 才能回收它的内核栈和 PCB。 */
            while (child->proc_on_cpu)
            {
                sched_schedule();
            }

            int16_t cpid = child->proc_pid;
            if (status)
            {
                /* POSIX wait status：低 7 位是把它杀死的信号号，为 0 才表示正常
                 * 退出（此时高 8 位是退出码）。ash 的 $? = 128 + signo 靠这个算 */
                *status = child->proc_exit_sig != 0
                              ? (child->proc_exit_sig & 0x7f)
                              : ((child->proc_exit_code & 0xff) << 8);
            }

            /* times() 的 tms_cutime：把子进程（及它已收割的孙辈）的累计执行时间
             * 归并到父进程。必须赶在下面 kfree(child) 之前，顺序反了就是读
             * 已释放内存。 */
            cur->proc_sum_exec_runtime_children +=
                child->proc_sum_exec_runtime + child->proc_sum_exec_runtime_children;

            /* 收割：释放它自己没法释放的内核栈与 PCB（还站在上面跑的时候不能
             * 自己拆），回收 PID */
            if (child->proc_sighand)
            {
                kfree(child->proc_sighand);
                child->proc_sighand = NULL;
            }
            dealloc_kernel_stack(child);
            dealloc_pid_map(cpid);
            kfree(child);

            return cpid;
        }

        if (!has_child)
        {
            cur->proc_state = RUNNING;
            return ENO17_NO_CHILD;
        }

        /* 有匹配的子进程但都还活着：WNOHANG 要求立刻返回 0（不是错误）。
         * 必须在置 INTERRUPTIBLE 之后、sched_schedule() 之前把状态改回来，
         * 否则就带着 INTERRUPTIBLE 返回用户态了——与下面 signal_pending 那条同型。 */
        if (options & WNOHANG)
        {
            cur->proc_state = RUNNING;
            return 0;
        }

        /* 有信号待处理就别睡了：把状态改回 RUNNING（不然就带着 INTERRUPTIBLE
         * 返回用户态了）并退回 -ERESTARTSYS，由投递点决定重启还是给 -EINTR。
         * do_wait 不用等待队列、只是置状态 + sched_schedule，所以不需要摘链。 */
        if (signal_pending(cur))
        {
            cur->proc_state = RUNNING;
            return ENO24_RESTARTSYS;
        }

        sched_schedule();
    }
}

/* ============================================================
 * 进程启动约定：argv / envp / auxv 初始栈
 * ============================================================ */

static int exec_args_init(exec_args_t *a)
{
    a->argc = 0;
    a->envc = 0;
    a->n = 0;
    a->used = 0;
    a->buf = kmalloc(EXEC_ARG_BUF_SIZE);
    return a->buf ? ENO0_NO_ERROR : ENO1_NOMORE_MEM;
}

static void exec_args_free(exec_args_t *a)
{
    if (a->buf)
    {
        kfree(a->buf);
        a->buf = NULL;
    }
}

/* 把一个内核字符串追加进暂存区（run_user_program 用，它没有用户态参数可读） */
static int exec_args_push(exec_args_t *a, const char *s)
{
    size_t n = strlen(s) + 1;
    if (a->n >= EXEC_MAX_ARGS || n > EXEC_ARG_STRLEN_MAX ||
        n > EXEC_ARG_BUF_SIZE - a->used)
    {
        return ENO27_ARG_TOO_LONG;
    }
    a->off[a->n] = a->used;
    memcpy(a->buf + a->used, s, n);
    a->used += n;
    a->n += 1;
    return ENO0_NO_ERROR;
}

/**
 * @brief 把用户空间的一个 NULL 结尾指针数组整体拷进暂存区
 * @param[in,out] a     暂存区
 * @param[in]     uvec  用户空间的 char *argv[] / char *envp[]；NULL 视为空数组
 * @param[out]    count 本次收进了几个串（调用方据此填 argc 或 envc）
 * @note 必须在切 satp 之前调用：argv 是二级指针，指针数组与它指向的字符串
 *   都在旧地址空间里。
 */
static int exec_args_copy_from_user(exec_args_t *a, char *const *uvec, int *count)
{
    *count = 0;
    if (uvec == NULL)
    {
        return ENO0_NO_ERROR;
    }
    while (1)
    {
        char *uptr = NULL;
        if (copy_from_user(&uptr, &uvec[*count], sizeof(uptr)) != 0)
        {
            return ENO8_NULL_POINTER;
        }
        if (uptr == NULL)
        {
            return ENO0_NO_ERROR;
        }
        if (a->n >= EXEC_MAX_ARGS)
        {
            return ENO27_ARG_TOO_LONG;
        }
        uint32_t room = EXEC_ARG_BUF_SIZE - a->used;
        if (room > EXEC_ARG_STRLEN_MAX)
        {
            room = EXEC_ARG_STRLEN_MAX;
        }
        long len = strncpy_from_user(a->buf + a->used, uptr, room);
        if (len == ENO11_NAME_TOO_LONG)
        {
            return ENO27_ARG_TOO_LONG; /* 串装不下，对 execve 而言就是 E2BIG */
        }
        if (len < 0)
        {
            return (int)len;
        }
        a->off[a->n] = a->used;
        a->used += (uint32_t)len + 1;
        a->n += 1;
        (*count)++;
    }
}

/**
 * @brief 在新地址空间的用户栈上铺好 argc / argv / envp / auxv
 * @param[in]  args    内核侧暂存的参数
 * @param[in]  info    elf_load 回吐的入口与程序头表信息
 * @param[out] sp_out  进入 _start 时的栈指针
 * @retval ENO0_NO_ERROR 成功
 * @retval ENO27_ARG_TOO_LONG 需要的空间超出了 64 KB 固定用户栈
 * @details 布局（低地址在上，即 sp 指向 argc）：
 *
 *   sp  -> argc
 *          argv[0..argc-1], NULL
 *          envp[0..envc-1], NULL
 *          auxv 若干对 (a_type, a_val)，以 AT_NULL 收尾
 *          ---- 填充 ----
 *          argv/envp 的字符串区
 *          16 字节 AT_RANDOM 种子
 *   USER_STACK_TOP
 *
 *   三条硬约束，写错的症状都离现场很远：
 *   1. sp 必须 16 字节对齐（riscv64 ABI）；
 *   2. argv/envp 里存的是指向字符串区的用户虚拟地址，所以要先定下字符串区
 *      的最终地址再填指针——实现上就是"先量尺寸、再填"两趟；
 *   3. argv[argc] 与 envp[envc] 的 NULL 都不能省：musl 靠 envp 的 NULL
 *      定位 auxv 的起点，少一个就是整个 auxv 错位、AT_PAGESZ 读成随机值。
 * @note 此刻已经切到新页表，写的是懒分配的用户栈 VMA。S 态带 SUM=1 访问用户地址
 *   触发的缺页由 vmm_page_fault_handler 正常服务，与 copy_to_user 走的是同一条路。
 */
static int setup_user_stack(const exec_args_t *args, const elf_info_t *info,
                            virAddr_t *sp_out)
{
    struct elf64_auxv aux[16];
    int na = 0;
    virAddr_t rand_va  = USER_STACK_TOP - 16;
    virAddr_t str_base = rand_va - args->used;

    aux[na].a_type = AT_PHDR;    aux[na++].a_val = info->phdr_va;
    aux[na].a_type = AT_PHENT;   aux[na++].a_val = info->phent;
    aux[na].a_type = AT_PHNUM;   aux[na++].a_val = info->phnum;
    aux[na].a_type = AT_PAGESZ;  aux[na++].a_val = PGSIZE;
    aux[na].a_type = AT_BASE;    aux[na++].a_val = 0;
    aux[na].a_type = AT_FLAGS;   aux[na++].a_val = 0;
    aux[na].a_type = AT_ENTRY;   aux[na++].a_val = info->entry;
    aux[na].a_type = AT_UID;     aux[na++].a_val = 0;
    aux[na].a_type = AT_EUID;    aux[na++].a_val = 0;
    aux[na].a_type = AT_GID;     aux[na++].a_val = 0;
    aux[na].a_type = AT_EGID;    aux[na++].a_val = 0;
    aux[na].a_type = AT_HWCAP;   aux[na++].a_val = 0;
    aux[na].a_type = AT_CLKTCK;  aux[na++].a_val = USER_HZ;
    aux[na].a_type = AT_SECURE;  aux[na++].a_val = 0;
    aux[na].a_type = AT_RANDOM;  aux[na++].a_val = rand_va;
    aux[na].a_type = AT_NULL;    aux[na++].a_val = 0;

    uint64_t ptr_bytes = sizeof(uint64_t)                       /* argc */
                       + (uint64_t)(args->argc + 1) * sizeof(uint64_t)
                       + (uint64_t)(args->envc + 1) * sizeof(uint64_t)
                       + (uint64_t)na * sizeof(struct elf64_auxv);
    virAddr_t sp = (str_base - ptr_bytes) & ~15UL;

    if (sp < USER_STACK_TOP - USER_STACK_LEN)
    {
        return ENO27_ARG_TOO_LONG;
    }

    /* 字符串区与 AT_RANDOM 种子。种子是弱熵（启动至今的纳秒数 + pid），
     * 只够喂 musl 的栈保护 canary，不可作密码学用途 */
    memcpy((void *)str_base, args->buf, args->used);
    uint64_t seed = ktime_get_ns() ^ ((uint64_t)proc_get_current()->proc_pid << 48);
    ((uint64_t *)rand_va)[0] = seed;
    ((uint64_t *)rand_va)[1] = seed * 6364136223846793005ULL + 1442695040888963407ULL;

    /* 指针区：argc、argv[]、NULL、envp[]、NULL、auxv */
    uint64_t *slot = (uint64_t *)sp;
    *slot++ = (uint64_t)args->argc;
    for (int i = 0; i < args->argc; i++)
    {
        *slot++ = str_base + args->off[i];
    }
    *slot++ = 0;
    for (int i = 0; i < args->envc; i++)
    {
        *slot++ = str_base + args->off[args->argc + i];
    }
    *slot++ = 0;
    memcpy(slot, aux, (size_t)na * sizeof(struct elf64_auxv));

    /* 自检：初始栈写错的症状离现场很远 */
    if (*(long *)sp != args->argc)
    {
        panic("setup_user_stack: argc readback mismatch (sp=%lx)", sp);
    }

    *sp_out = sp;
    return ENO0_NO_ERROR;
}

/* 造一个"sret 即从 entry 开始执行新程序"的 trap 帧，除 sp 外的通用寄存器全为 0。
 * 用户的 tp 是 TLS 指针，不是 hart 号；hart 号由内核栈顶的保留槽维持（见 KSTACK_RESERVED）。
 * sstatus 每一位都要想清楚。SIE 必须清：trap_return 会在 sret 前把它整个写回，中断若在
 * sscratch 已置为栈顶之后被打开，时钟中断会被误判成来自 U 态、新帧压在本帧上
 * （sret 用 SPIE 恢复中断）；在 syscall trap 里硬件已清过 SIE，这里照样清，不让这条不变式
 * 依赖"调用者恰好在 trap 上下文里"。FS 置 Initial，用户态可以直接用浮点。 */
static void user_trapframe_init(intstkf_t *f, virAddr_t entry, virAddr_t ustack)
{
    memset(f, 0, sizeof(intstkf_t));
    f->sepc    = entry;
    f->x2_sp   = ustack;
    f->sstatus = (read_csr(sstatus) & ~SSTATUS_SPP & ~SSTATUS_SIE & ~SSTATUS_FS)
                 | SSTATUS_SPIE | SSTATUS_SUM | SSTATUS_FS_INITIAL;
}

/**
 * @brief 把整个可执行文件读进内核堆
 * @param[in]  path     绝对路径
 * @param[out] img_out  成功时指向 kmalloc 出的镜像，由调用方 kfree
 * @param[out] size_out 文件字节数；打不开时不写
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO5_NOSUCH_ENTRY 打不开
 * @retval ENO6_INVAL_PARAM  空文件或短读
 * @retval ENO1_NOMORE_MEM   内存不足
 */
static int read_exec_image(const char *path, unsigned char **img_out, off_t *size_out)
{
    vfs_lock();
    file_t *f = vfs_open(path, O_RDONLY, NULL);
    if (f == NULL)
    {
        vfs_unlock();
        return ENO5_NOSUCH_ENTRY;
    }

    int ret = ENO0_NO_ERROR;
    unsigned char *img = NULL;
    off_t size = vfs_lseek(f, 0, SEEK_END);
    vfs_lseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        ret = ENO6_INVAL_PARAM;
    }
    else if ((img = kmalloc((size_t)size)) == NULL)
    {
        ret = ENO1_NOMORE_MEM;
    }
    else if (vfs_read(f, img, (size_t)size) != (ssize_t)size)
    {
        kfree(img);
        img = NULL;
        ret = ENO6_INVAL_PARAM;
    }
    vfs_close(f);
    vfs_unlock();

    *size_out = size;
    *img_out = img;
    return ret;
}

/**
 * @brief 用 path 指向的 ELF 替换当前进程的地址空间（execve 语义：换脑不换壳）
 * @param[in,out] sp   当前 syscall 的 trap 帧；成功时被改写为"进入新程序"的帧
 * @param[in]     path 用户空间的程序路径字符串
 * @param[in]     argv 用户空间的参数向量，NULL 视为空
 * @param[in]     envp 用户空间的环境向量，NULL 视为空
 * @retval ENO0_NO_ERROR 成功——trap 帧已指向新程序，返回后 sret 即进入新程序，旧程序视角看不到此返回值
 * @retval <0 失败（负 ENO*）——旧地址空间原封不动，exec 失败不致命，返回值传回旧程序
 * @details 保留 PCB / PID / 父子关系 / fd 表 / cwd，只把地址空间整个换掉。顺序极其关键：
 *   1. 先把 path / argv / envp 从用户空间拷进内核（切 satp 后用户指针失效）；
 *   2. 先把整个 ELF 读进内核堆（内核偏移映射，切 satp 后仍可达）；
 *   3. 建新 mm、切到新地址空间（旧 mm 先留着，加载失败要回滚）；
 *   4. elf_load 到新 mm；失败则切回旧 mm、销毁半成品新 mm、返回错误；
 *   5. 成功后才销毁旧 mm（此刻已不站在它的页表上），随即关闭带 FD_CLOEXEC 的 fd；
 *   6. 建全新用户栈 VMA 并在上面铺 argc/argv/envp/auxv；fd 表其余部分原样保留；
 *   7. 改写 trap 帧（sepc=入口、sp=铺好的栈指针、清通用寄存器），走正常 syscall 返回路径进入新程序。
 * @note FD_CLOEXEC 的 fd 在第 5 步（销毁旧 mm 之后）才关闭，而不是一进函数就关——第 1-4 步
 *   随时可能失败并回滚到旧程序继续执行，那种情况下 fd 表必须原封不动（POSIX 语义：
 *   execve 失败等价于没发生过）；只有确认新程序已经站稳（旧 mm 已销毁、没有回头路）之后，
 *   关闭 CLOEXEC fd 才是安全的。
 * @note 成功路径不能让 trap.c 把返回值写回 a0——第 7 步刚把整个 trap 帧清零并按新程序
 *   构造好，a0 位置存的已经是新程序的初始寄存器值。sys_execve 用返回值区分两种情形。
 */
int do_exec(intstkf_t *sp, const char *path, char *const *argv, char *const *envp)
{
    pcb_t *cur = proc_get_current();

    /* 1) 路径与参数都来自用户空间：切 satp 前全部拷进内核缓冲 */
    char kpath[VFS_PATH_MAX];
    long path_len = strncpy_from_user(kpath, path, sizeof(kpath));
    if (path_len < 0)
    {
        return (int)path_len;
    }

    exec_args_t args;
    int ret = exec_args_init(&args);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    ret = exec_args_copy_from_user(&args, argv, &args.argc);
    if (ret == ENO0_NO_ERROR)
    {
        ret = exec_args_copy_from_user(&args, envp, &args.envc);
    }
    if (ret != ENO0_NO_ERROR)
    {
        exec_args_free(&args);
        return ret;
    }

    /* 2) 把整个 ELF 读进内核堆 */
    unsigned char *img;
    off_t size;
    ret = read_exec_image(kpath, &img, &size);
    if (ret != ENO0_NO_ERROR)
    {
        exec_args_free(&args);
        return ret;
    }

    /* 3) 建新地址空间并切过去（旧 mm 先留着）*/
    mm_t *old_mm = cur->proc_mm;
    mm_t *new_mm = create_user_mm();
    if (new_mm == NULL)
    {
        kfree(img);
        exec_args_free(&args);
        return ENO1_NOMORE_MEM; /* 旧地址空间原封不动 */
    }
    cur->proc_mm = new_mm;
    cur->proc_context.satp = SATPMODE_RV39 | new_mm->pgd_ppn;
    write_csr(satp, cur->proc_context.satp);
    tlb_flush_all();

    /* 4) 解析 ELF 到新地址空间 */
    elf_info_t einfo;
    ret = elf_load(new_mm, img, (uint64_t)size, &einfo);
    kfree(img);
    if (ret != ENO0_NO_ERROR)
    {
        /* 加载失败：切回旧地址空间、销毁半成品新 mm，返回错误（exec 失败不致命）*/
        cur->proc_mm = old_mm;
        cur->proc_context.satp = SATPMODE_RV39 | old_mm->pgd_ppn;
        write_csr(satp, cur->proc_context.satp);
        tlb_flush_all();
        vmm_mm_destroy(new_mm);
        exec_args_free(&args);
        return ret;
    }

    /* 5) 成功——此刻站在新页表上，旧 mm 已非活动，安全销毁；exec 已无回头路，
     *    此时才能安全关闭带 FD_CLOEXEC 的 fd（理由见函数级注释） */
    vmm_mm_destroy(old_mm);
    proc_fd_close_on_exec(cur);
    /* 信号：被捕获的动作回到 SIG_DFL（旧 handler 地址属于刚被销毁的地址空间），
     * SIG_IGN / SIG_DFL 与屏蔽字 proc_sig_mask 原样保留。位置与
     * proc_fd_close_on_exec 相同，理由也相同：exec 失败必须等价于没发生过 */
    signal_hand_reset_on_exec(cur);
    /* ITIMER_REAL：POSIX 要求 execve 清空定时器（umask 相反，原样保留） */
    ktime_alarm_cancel(cur);

    /* 6) 全新用户栈 VMA（懒分配）并在上面铺 argc/argv/envp/auxv；fd 表其余部分原样保留 */
    vma_t *stk = vmm_vma_create(USER_STACK_TOP - USER_STACK_LEN, USER_STACK_TOP, VMP_R | VMP_W);
    if (stk == NULL)
    {
        panic("do_exec: no memory for the user stack VMA after point of no return");
    }
    vmm_vma_insert(new_mm, stk);

    virAddr_t user_sp;
    ret = setup_user_stack(&args, &einfo, &user_sp);
    exec_args_free(&args);
    if (ret != ENO0_NO_ERROR)
    {
        /* 走不到：参数总量已被 EXEC_ARG_BUF_SIZE(4KB) + EXEC_MAX_ARGS(64) 夹住，
         * 最坏也就 5 KB 出头，远小于 64 KB 的用户栈。真到了这里也已经没有回头路
         * ——旧 mm 已销毁，返回错误等于让旧程序拿着一个空地址空间继续跑。 */
        panic("do_exec: setup_user_stack failed after point of no return, ret=%d", ret);
    }

    /* 7) 改写当前 trap 帧：sret 直接进入新程序（复用 syscall 返回路径，不另起 enter_user_mode）*/
    user_trapframe_init(sp, einfo.entry, user_sp);

    return ENO0_NO_ERROR;
}

/**
 * @brief fork 一个执行 func(args) 的内核线程（与父进程共享地址空间）
 */
int create_kernel_thread_by_fork(void *func(void *), void *args, uint32_t clone_flags)
{
    intstkf_t regs;
    memset(&regs, 0, sizeof(intstkf_t));
    regs.x8_s0 = (uint64_t)func;
    regs.x9_s1 = (uint64_t)args;
    /* SPP=1 返回 S 态；SPIE=1 返回后开中断；SIE=0 模拟"在 trap 里"的环境 */
    regs.sstatus = ((read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE & ~SSTATUS_FS)
                   | SSTATUS_FS_INITIAL;
    regs.sepc = (uint64_t)kernel_thread_entry;
    return do_fork((clone_flags | CLONE_VM), 0, &regs);
}

/**
 * @brief 进程子系统的全局结构初始化，只由 hart0 调用一次
 * @note 必须在启动从核之前调用。从 proc_init() 里拆出来的理由：从核一上电就会跑自己的
 *   proc_init()，那里会取 proc_list_lock；锁若留在 proc_init() 里初始化，就可能在被
 *   别的核持有时被清零。
 */
void proc_early_init(void)
{
    spinlock_init(&proc_list_lock);
    INIT_LIST_HEAD(&proc_list);
    spinlock_init(&pid_lock);
}

/**
 * @brief 每个 hart 建自己的 idle 并登记为当前任务；hart0 另外 fork 出 init（pid 1）
 */
void proc_init(void)
{
    pcb_t *idle = create_first_proc_idle();
    if (idle == NULL)
    {
        panic("cannot allocate the idle proc");
    }
    cpu_get_current()->idle_proc = idle;
    sched_set_current(idle);

    if (cpu_get_core_id() == 0)
    {
        int id_init = create_kernel_thread_by_fork((void *)init, NULL, 0);
        pcb_t *pcb_init = find_proc_by_pid(id_init);
        const char *name = "init";
        set_proc_name(pcb_init, name);
    }
}

static pcb_t *alloc_new_proc(void)
{
    pcb_t *pcb = slab_cache_alloc(pcb_cache);
    if (pcb != NULL)
    {
        pcb->kernel_stack = 0;
        memset(&(pcb->proc_context), 0, sizeof(ctx_t));
        /* 内核线程默认使用内核页表；用户进程 fork 时由 copy_proc_mm 覆盖 */
        pcb->proc_context.satp = SATPMODE_RV39 | vmm_kernel_pgd_ppn;
        pcb->proc_int_stack = NULL;
        pcb->proc_mm = NULL;
        pcb->proc_parent = NULL;
        pcb->proc_pid = ENO3_NOFREE_PID;
        memset(pcb->proc_pname, 0, sizeof(pcb->proc_pname));
        pcb->proc_state = UNINIT;
        pcb->need_resched = false;
        pcb->proc_cwd = NULL;  /* NULL 表示当前工作目录为 VFS 根目录 */

        /* 进程生命周期：父子链由 do_fork 挂接，此处先建空链表头与初始退出码 */
        pcb->proc_exit_code = 0;
        INIT_LIST_HEAD(&(pcb->proc_children));
        INIT_LIST_HEAD(&(pcb->proc_sibling_linker));

        memset(pcb->proc_fds, 0, sizeof(pcb->proc_fds));
        memset(pcb->proc_fd_flags, 0, sizeof(pcb->proc_fd_flags));

        /* 信号：默认是内核线程的形状（不接收信号）。用户进程的 sighand 由
         * do_fork（继承父进程）或 proc_signal_init_user（第一个用户进程）补上 */
        pcb->proc_sighand = NULL;
        pcb->proc_sig_pending = 0;
        pcb->proc_sig_mask = 0;
        pcb->proc_syscall_orig_a0 = 0;
        pcb->proc_exit_sig = 0;
        pcb->proc_pgid = 0;

        pcb->proc_sched_class = &fair_sched_class;
        pcb->proc_policy = SCHED_NORMAL;
        pcb->proc_on_rq = false;
        pcb->proc_on_cpu = false;
        pcb->proc_nice = 0;
        pcb->proc_weight = SCHED_NICE_0_WEIGHT;
        /* 新任务第一次被换上时，fork_out 里的 sched_finish_switch 要用它把中断打开 */
        pcb->proc_rq_key = true;
        pcb->proc_vruntime = 0;
        pcb->proc_vruntime_rem = 0;
        pcb->proc_exec_start = 0;
        pcb->proc_sum_exec_runtime = 0;
        pcb->proc_sum_exec_runtime_prev = 0;
        pcb->proc_rt_priority = 0;
        INIT_LIST_HEAD(&(pcb->proc_rt_linker));
        RB_CLEAR_NODE(&(pcb->proc_rbtree_node));

        INIT_LIST_HEAD(&(pcb->proc_list_linker));
        INIT_LIST_HEAD(&(pcb->proc_wait_linker));

        pcb->proc_wake_time_ns = 0;
        INIT_LIST_HEAD(&(pcb->proc_timer_linker));

        /* ITIMER_REAL：POSIX 要求 fork 的子进程不继承定时器，所以这里一律清空，
         * do_fork 也不去复制父进程的这三个字段 */
        pcb->proc_alarm_expire_ns = 0;
        pcb->proc_alarm_interval_ns = 0;
        INIT_LIST_HEAD(&(pcb->proc_alarm_linker));

        /* umask 相反：fork 要继承、exec 要保留，所以这里给的只是"没有父进程时"的默认值，
         * do_fork 会用父进程的覆盖掉 */
        pcb->proc_umask = 0022;
        pcb->proc_clear_child_tid = 0;
        pcb->proc_sum_exec_runtime_children = 0;
    }
    return pcb;
}

static int16_t alloc_kernel_stack(pcb_t *pcb)
{
    phyAddr_t *kernel_stack_page_addr = kmalloc(KERNRL_STKSIZE);
    if (kernel_stack_page_addr != NULL)
    {
        pcb->kernel_stack = (phyAddr_t)kernel_stack_page_addr;
        return ENO0_NO_ERROR;
    }
    return ENO1_NOMORE_MEM;
}

static int16_t dealloc_kernel_stack(pcb_t *pcb)
{
    kfree((void *)(pcb->kernel_stack));
    return ENO0_NO_ERROR;
}

static int16_t copy_proc_mm(uint32_t clone_flags, pcb_t *pcb)
{
    /* 内核线程fork，子线程共享父线程mm */
    if (clone_flags & CLONE_VM)
    {
        pcb->proc_mm = proc_get_current()->proc_mm;
        return ENO0_NO_ERROR;
    }

    mm_t *mm = vmm_mm_create();
    if (!mm)
    {
        panic("out of memory for the child mm");
    }

    /* 必须先建立独立 PGD 并设好 mm->pgd_ppn，vmm_mm_copy 才能向正确的页表写 PTE */
    if (vmm_mm_alloc_pgd(mm) != ENO0_NO_ERROR)
    {
        panic("out of memory for the child PGD");
    }

    vmm_mm_copy(mm, proc_get_current()->proc_mm);

    pcb->proc_mm = mm;
    pcb->proc_context.satp = SATPMODE_RV39 | mm->pgd_ppn;

    return ENO0_NO_ERROR;
}

static void copy_proc_stk(pcb_t *pcb, uintptr_t stack, intstkf_t *regs)
{
    pcb->proc_int_stack = (intstkf_t *)(PROC_KSTACK_TOP(pcb) - sizeof(intstkf_t));
    *(pcb->proc_int_stack) = *(regs);
    /* 子进程里 fork 返回 0 */
    pcb->proc_int_stack->x10_a0 = 0;
    pcb->proc_int_stack->x2_sp = (stack == 0) ? (uintptr_t)pcb->proc_int_stack : stack;
    pcb->proc_context.x1_ra = (uint64_t)fork_out;
    pcb->proc_context.x2_sp = (uint64_t)pcb->proc_int_stack;
}

static pcb_t *create_first_proc_idle(void)
{

    pcb_t *idle = alloc_new_proc();
    if (idle != NULL)
    {
        idle->proc_pid = 0;
        /* alloc_new_proc 默认把所有任务挂 &fair_sched_class，idle 单独覆盖成 &idle_sched_class */
        idle->proc_sched_class = &idle_sched_class;
        /* 每个核用自己在 startup.S 里的那一格启动栈作内核栈，按逻辑 cpu 号索引 */
        idle->kernel_stack = (phyAddr_t)cpu_boot_stack_top((uint16_t)cpu_get_core_id());
        idle->proc_state = RUNNING;
        idle->need_resched = true;
        idle->proc_cwd = NULL;  /* idle 进程使用 VFS 根目录 */
        const char *name = "idle";
        set_proc_name(idle, name);
        /* 各 hart 并发建自己的 idle，task_count 的自增必须串行化 */
        irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
        task_count = task_count + 1;
        spinlock_release(&proc_list_lock, plist_key);
    }
    return idle;
}

/* @note 调用者不得持有 proc_list_lock（自旋锁不可重入）。 */
static pcb_t *find_proc_by_pid(int16_t pid)
{
    if (0 < pid && pid <= PID_MAX_VALUE)
    {
        struct list_head *currentProc;
        pcb_t *currentPcb;
        irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
        list_for_each(currentProc, &proc_list)
        {
            currentPcb = list_entry(currentProc, pcb_t, proc_list_linker);
            if (currentPcb->proc_pid == pid)
            {
                spinlock_release(&proc_list_lock, plist_key);
                return currentPcb;
            }
        }
        spinlock_release(&proc_list_lock, plist_key);
    }
    return NULL;
}

/**
 * @brief 当前进程的父进程 pid；没有父进程时返回 0
 * @note 在 proc_list_lock 下读：父进程可能正在退出、随即被收割释放。
 */
int16_t proc_get_ppid(void)
{
    irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
    pcb_t *p = proc_get_current()->proc_parent;
    int16_t ppid = (p != NULL) ? (int16_t)p->proc_pid : 0;
    spinlock_release(&proc_list_lock, plist_key);
    return ppid;
}

/**
 * @brief 进程 pid 所在的进程组
 * @retval >=0 进程组号
 * @retval ENO25_NO_SUCH_PROC 没有这个进程
 */
int16_t proc_get_pgid(int16_t pid)
{
    int16_t pgid = ENO25_NO_SUCH_PROC;
    irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_list_linker);
        if (p->proc_pid == pid)
        {
            pgid = p->proc_pgid;
            break;
        }
    }
    spinlock_release(&proc_list_lock, plist_key);
    return pgid;
}

/**
 * @brief 按 pid 查找 pcb（find_proc_by_pid 的公开包装），未找到返回 NULL
 */
pcb_t *proc_find_by_pid(int16_t pid)
{
    return find_proc_by_pid(pid);
}

/**
 * @brief 在 proc_list_lock 下找到 pid 对应的进程并立刻对它调用 fn
 */
bool proc_apply_by_pid(int16_t pid, void (*fn)(pcb_t *p, int arg), int arg)
{
    bool found = false;
    irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_list_linker);
        if (p->proc_pid == pid)
        {
            found = true;
            fn(p, arg);
            break;
        }
    }
    spinlock_release(&proc_list_lock, plist_key);
    return found;
}

/**
 * @brief 在 proc_list_lock 下对进程组 pgid 的每个成员调用 fn，返回成员个数
 */
int proc_apply_by_pgid(int16_t pgid, void (*fn)(pcb_t *p, int arg), int arg)
{
    int count = 0;
    irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
    struct list_head *pos;
    list_for_each(pos, &proc_list)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_list_linker);
        if (p->proc_pgid == pgid)
        {
            count++;
            fn(p, arg);
        }
    }
    spinlock_release(&proc_list_lock, plist_key);
    return count;
}

/* 从上次分配处往后找下一个空闲 pid，用满 [1, PID_MAX_VALUE] 后回绕——刚释放的号
 * 要等一整圈才会再发出去，按 pid 操作的 wait/kill 不容易误伤复用了旧号的新进程 */
static int16_t alloc_pid_map(void)
{
    static uint16_t last_alloc = 0;
    int16_t ret = ENO3_NOFREE_PID;

    irq_key_t pid_lock_key = spinlock_acquire(&pid_lock);
    for (uint16_t tried = 0; tried < PID_MAX_VALUE; tried++)
    {
        last_alloc = (last_alloc % PID_MAX_VALUE) + 1;
        uint64_t bit = 1UL << (last_alloc % 64);
        if ((pid_bitmap[last_alloc / 64] & bit) == 0)
        {
            pid_bitmap[last_alloc / 64] |= bit;
            ret = (int16_t)last_alloc;
            break;
        }
    }
    spinlock_release(&pid_lock, pid_lock_key);
    return ret;
}

static void dealloc_pid_map(int16_t pid)
{
    irq_key_t pid_lock_key = spinlock_acquire(&pid_lock);
    pid_bitmap[pid / 64] &= ~(1UL << (pid % 64));
    spinlock_release(&pid_lock, pid_lock_key);
}

/**
 * @brief 每个 hart 的 idle 循环：有活就调度过去，没活就 wfi
 */
void idle(void)
{
    while (1)
    {
        /* 每轮都调度，不靠 need_resched 当门槛：本 hart 空转时没人会替它置这个标志，
         * 那样它永远看不到别的 hart 刚放进共享就绪队列的任务。 */
        sched_schedule();

        /* 挑完还是自己的 idle_proc，说明真的没活干，wfi 休眠。唤醒有两条路径：
         * 别的 hart 往就绪队列放任务时经 sched_activate() 主动发来的 IPI（即时），
         * 以及 tick 中断（最坏一个 tick）兜底。 */
        if (proc_get_current() == cpu_get_current()->idle_proc)
        {
            asm volatile("wfi");
        }
    }
}

/**
 * @brief 建一个独立的用户地址空间（新 PGD + 复制内核高半段）
 * @return 新 mm；失败返回 NULL
 * @details 不切 satp、不挂到任何 pcb——由调用方决定何时切换
 *   （proc_run_user_program 首次进入 / do_exec 换脑）。
 */
static mm_t *create_user_mm(void)
{
    mm_t *mm = vmm_mm_create();
    if (mm == NULL)
    {
        return NULL;
    }
    if (vmm_mm_alloc_pgd(mm) != ENO0_NO_ERROR)
    {
        return NULL; /* OOM 极端边界：此处不回收 mm（系统已濒临耗尽），可接受 */
    }

    /* 每个用户地址空间都要有 sigpage，否则装了 handler 的进程一从 handler 返回
     * 就是取指缺页。放在这里而不是各调用点，exec 与首个用户程序两条路径自动都有 */
    if (signal_map_sigpage(mm) != ENO0_NO_ERROR)
    {
        return NULL; /* OOM 极端边界：同上，不回收半成品 mm */
    }
    return mm;
}

/**
 * @brief 让一个内核线程变成"能接收信号的用户进程"
 * @details init 是 create_kernel_thread_by_fork 造出来的，proc_sighand 为 NULL；
 *   它 run_user_program 变身用户进程之后必须补上，否则整棵进程树都收不到信号。
 *   进程组初值取自己的 pid——它就是前台进程组的组长。
 */
static void proc_signal_init_user(pcb_t *p)
{
    if (p->proc_sighand == NULL)
    {
        p->proc_sighand = signal_hand_create();
        if (p->proc_sighand == NULL)
        {
            panic("proc_signal_init_user: cannot allocate sighand");
        }
    }
    p->proc_sig_pending = 0;
    p->proc_sig_mask = 0;
    p->proc_pgid = p->proc_pid;
}

/**
 * @brief 建独立用户地址空间、从根文件系统加载指定程序并进入 U 态
 * @param[in] path 可执行文件在根文件系统里的绝对路径
 * @details 内核线程变身用户进程的加载路径，程序来自 rootfs 镜像。读法与 `do_exec` 一致：一次 kmalloc 把整个文件读进内核堆，`elf_load` 把各段拷进
 *   用户页之后立刻归还。区别只在参数来源——`do_exec` 的 path/argv 来自用户空间要
 *   `copy_from_user`，这里的是内核里的字面量。
 * @note argv 由调用方给全，含 argv[0]——BusyBox 靠 argv[0] 分发 applet。
 * @note noreturn：`enter_user_mode` 内部 `sret` 进入 U 态，不会返回
 * @note 找不到文件时 panic 并提示跑 `make rootfs`：这条路径上没有可降级的余地，
 *   静默失败只会表现成"内核起来了但什么都没发生"。
 */
void proc_run_user_program(const char *path, const char *const argv[], int argc)
{
    /* 1) 独立用户 mm（新 PGD + 复制内核半段） */
    mm_t *mm = create_user_mm();
    if (mm == NULL)
    {
        panic("run_user_program: create_user_mm failed");
    }

    /* 2) 切到用户地址空间（page fault 用当前进程的 proc_mm，必须先挂上） */
    pcb_t *cur = proc_get_current();
    cur->proc_mm = mm;
    cur->proc_context.satp = SATPMODE_RV39 | mm->pgd_ppn;
    write_csr(satp, cur->proc_context.satp);
    tlb_flush_all();

    /* 3) 从根文件系统读出整个 ELF，解析：按 PT_LOAD 段建 VMA、映射、拷贝内容 */
    unsigned char *img;
    off_t size = 0;
    int ret = read_exec_image(path, &img, &size);
    if (ret == ENO5_NOSUCH_ENTRY)
    {
        panic("run_user_program: cannot open %s (rootfs image missing? run `make rootfs`)", path);
    }
    if (ret == ENO1_NOMORE_MEM)
    {
        panic("run_user_program: no memory for %s (%ld bytes)", path, (long)size);
    }
    if (ret != ENO0_NO_ERROR)
    {
        if (size <= 0)
        {
            panic("run_user_program: %s is empty", path);
        }
        panic("run_user_program: short read on %s", path);
    }

    elf_info_t einfo;
    ret = elf_load(mm, img, (uint64_t)size, &einfo);
    /* elf_load 已把各段内容拷进用户页，缓冲区可以还了 */
    kfree(img);
    if (ret != ENO0_NO_ERROR)
    {
        panic("run_user_program: elf_load failed on %s, ret=%d", path, ret);
    }

    /* 4) 用户栈 VMA（懒分配，首次访问由 page fault 落实），并铺上初始栈。
     * 建栈代码与 do_exec 共用同一份，两条路径同时被测到。 */
    vma_t *stk = vmm_vma_create(USER_STACK_TOP - USER_STACK_LEN, USER_STACK_TOP, VMP_R | VMP_W);
    vmm_vma_insert(mm, stk);

    exec_args_t args;
    if (exec_args_init(&args) != ENO0_NO_ERROR)
    {
        panic("run_user_program: exec_args_init failed");
    }
    for (int i = 0; i < argc; i++)
    {
        if (exec_args_push(&args, argv[i]) != ENO0_NO_ERROR)
        {
            panic("run_user_program: exec_args_push failed on argv[%d]", i);
        }
    }
    args.argc = argc;
    args.envc = 0;
    virAddr_t user_sp;
    if (setup_user_stack(&args, &einfo, &user_sp) != ENO0_NO_ERROR)
    {
        panic("run_user_program: setup_user_stack failed");
    }
    exec_args_free(&args);

    /* 5) 装 stdin/stdout/stderr（fd 0/1/2）；fork 出的子进程由 do_fork 的 proc_fd_copy 继承 */
    proc_install_stdio();

    /* 6) 信号处理表：init 变身用户进程之后才需要，fork 出的子进程由 do_fork 继承。
     * 顺带把它设成 TTY 的前台进程组——否则 ^C 打给的还是初值 1（init 的 pgid），
     * 而 init 是内核线程，收不到信号，按下去什么也不会发生。真实系统里这一步由
     * shell 的 tcsetpgrp 接管，在那之前"第一个用户进程就是前台作业"是对的。 */
    proc_signal_init_user(cur);
    tty_set_foreground_pgid(cur->proc_pgid);

    /* 7) 进入 U 态 */
    enter_user_mode(einfo.entry, user_sp);
}

static int16_t init(void)
{
    printf("init: pid 1 started\n");

#if DEBUG_SUITE
    debug_suite_run();
    return 0;
#else
    /* PID 1 自己变身 /sbin/init：孤儿过继目标仍是 find_proc_by_pid(1)，收割随之搬到用户态 */
    static const char *const init_argv[] = { "init" };
    proc_run_user_program("/sbin/init", init_argv, 1);
    panic("init: run_user_program(/sbin/init) returned");
#endif
}

static void fork_out(void)
{
    /* 新执行流第一次被 switch_to() 换上：按"接力"约定由它补上 sched_schedule() 欠下的
     * 那次放锁（key 取自 proc_rq_key，alloc_new_proc 已初始化成 true），漏掉就是调度器死锁。 */
    sched_finish_switch();

    fork_out_asm(proc_get_current()->proc_int_stack);
}

#if DEBUG_PROC_CTXSTK
static void print_ctx_stk(ctx_t *ctx)
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
    printf("  satp     0x%08lx\n", (ctx->satp));
    printf("=================================================================\n");
}
#endif

/**
 * @brief 将当前内核线程变身为用户态进程，跳转到用户入口执行
 * @param[in] entry  用户程序入口虚拟地址（将写入 sepc，sret 后 PC 跳至此处）
 * @param[in] ustack 用户栈顶虚拟地址（将写入帧的 x2_sp，须 16 字节对齐）
 * @details 在内核栈顶（PROC_KSTACK_TOP）伪造一个 trap 帧：sepc=entry、sp=ustack、tp=0、
 *   sstatus 置 SPP=0 / SPIE=1 / SUM=1，再经 fork_out_asm → trap_return → sret 进入 U 态。
 *   sscratch 由 trap_return 在关中断的尾段设好，这里不碰。
 *
 * @note 此函数不返回（标注 __attribute__((noreturn))）。
 *   调用前须确保：
 *     - 当前进程的 proc_mm 已挂载用户地址空间且 satp 已切换；
 *     - entry 所在代码页和 ustack 所在栈 VMA 已就绪（可为懒分配，首次访问触发 page fault）；
 *     - trap_init 已置 sstatus.SUM=1，内核可直接读写用户页。
 */
void enter_user_mode(virAddr_t entry, virAddr_t ustack)
{
    pcb_t *cur = proc_get_current();
    intstkf_t *f = (intstkf_t *)(PROC_KSTACK_TOP(cur) - sizeof(intstkf_t));

    user_trapframe_init(f, entry, ustack);
    /* 不能在这里写 sscratch：它在 S 态必须为 0，trap_entry 靠它判断 trap 来自 U 态。
     * trap_return 在 SPP==0 分支、关中断的尾段会算出同样的值。 */
    fork_out_asm(f);
}

/**
 * @brief 在当前进程的 fd 表中找最小可用的文件描述符下标
 * @return 成功返回 [0, NOFILE) 内的下标；fd 表已满返回 ENO18_TOO_MANY_FILES
 * @note 只负责挑号，不写入 fd 表——真正把 file_t 装进去是 proc_fd_install() 的职责，
 *   两步拆分参照 Linux get_unused_fd()/fd_install()。
 */
int proc_fd_alloc(void)
{
    return proc_fd_alloc_from(0);
}

/**
 * @brief 从指定下标起找最小的空闲 fd
 * @param[in] from 起始下标（fcntl 的 F_DUPFD 要求"不小于 arg 的最小空闲 fd"）
 * @retval >=0 找到的空闲 fd
 * @retval ENO18_TOO_MANY_FILES 没有空闲槽位
 * @retval ENO6_INVAL_PARAM     from 越界
 */
int proc_fd_alloc_from(int from)
{
    if (from < 0 || from >= NOFILE)
    {
        return ENO6_INVAL_PARAM;
    }

    file_t **fds = proc_get_current()->proc_fds;
    for (int i = from; i < NOFILE; i++)
    {
        if (fds[i] == NULL)
        {
            return i;
        }
    }

    return ENO18_TOO_MANY_FILES;
}

/**
 * @brief 按 fd 查询当前进程已打开的 file_t
 * @param[in] fd 文件描述符
 * @return 对应的 file_t 指针；fd 越界或该槽位为空（未打开）均返回 NULL
 */
file_t *proc_fd_get(int fd)
{
    if (fd < 0 || fd >= NOFILE)
    {
        return NULL;
    }

    return proc_get_current()->proc_fds[fd];
}

/**
 * @brief 把 file_t 装入当前进程 fd 表的指定槽位
 * @param[in] fd 目标文件描述符，通常来自 proc_fd_alloc() 的返回值
 * @param[in] f  要装入的 file_t
 * @retval ENO0_NO_ERROR    成功
 * @retval ENO6_INVAL_PARAM fd 越界（不在 [0, NOFILE) 内）
 * @note 直接覆盖目标槽位原有内容，不会先关闭旧的 file_t——调用方需自行保证
 *   该槽位是空闲的（比如刚由 proc_fd_alloc() 分配出来）。
 */
int proc_fd_install(int fd, file_t *f)
{
    if (fd < 0 || fd >= NOFILE)
    {
        return ENO6_INVAL_PARAM;
    }

    pcb_t *cur = proc_get_current();
    cur->proc_fds[fd] = f;
    cur->proc_fd_flags[fd] = 0; /* 新装入的 fd 默认不带 flag，需要的话调用方另调 proc_fd_set_flags */

    return ENO0_NO_ERROR;
}

/**
 * @brief 设置某个 fd 的标志位
 * @param[in] fd    目标文件描述符
 * @param[in] flags 覆盖写入的标志位组合
 * @note fd 越界或该槽位未打开时静默忽略——调用方（openat/fcntl）应已先校验过 fd 有效。
 */
void proc_fd_set_flags(int fd, uint8_t flags)
{
    if (fd < 0 || fd >= NOFILE)
    {
        return;
    }

    pcb_t *cur = proc_get_current();
    if (cur->proc_fds[fd] == NULL)
    {
        return;
    }
    cur->proc_fd_flags[fd] = flags;
}

/**
 * @brief 读取某个 fd 的标志位
 * @param[in] fd 目标文件描述符
 * @return 该 fd 的标志位；fd 越界或槽位未打开时返回 0
 * @note 调用方（fcntl F_GETFD）应已先用 proc_fd_get() 确认 fd 有效，
 *   本函数对无效 fd 返回 0 而非报错，是为了让调用点保持简单。
 */
uint8_t proc_fd_get_flags(int fd)
{
    if (fd < 0 || fd >= NOFILE)
    {
        return 0;
    }

    pcb_t *cur = proc_get_current();
    if (cur->proc_fds[fd] == NULL)
    {
        return 0;
    }
    return cur->proc_fd_flags[fd];
}

/**
 * @brief 关闭当前进程的一个文件描述符
 * @param[in] fd 要关闭的文件描述符
 * @retval ENO0_NO_ERROR     成功（含引用计数减一但未归零、底层未真正释放的情况）
 * @retval ENO6_INVAL_PARAM  fd 越界
 * @retval ENO8_NULL_POINTER 该 fd 对应槽位本就是空的（未打开）
 * @details 调用 vfs_close() 递减 file_t 引用计数（fork 后父子共享同一个 file_t，
 *   计数归零才真正释放底层资源），随后无条件把 fd 表槽位清 NULL——即使
 *   vfs_close() 内部因引用计数未归零而没有释放，这个 fd 号对当前进程也已经
 *   失效，必须清空，否则 proc_fd_alloc() 会一直认为它被占用。
 */
int proc_fd_close(int fd)
{
    if (fd < 0 || fd >= NOFILE)
    {
        return ENO6_INVAL_PARAM;
    }

    pcb_t *cur = proc_get_current();
    file_t *f = cur->proc_fds[fd];
    bool need_lock = vfs_file_needs_lock(f);
    if (need_lock)
    {
        vfs_lock();
    }
    int ret = vfs_close(f);
    if (need_lock)
    {
        vfs_unlock();
    }
    cur->proc_fds[fd] = NULL;
    cur->proc_fd_flags[fd] = 0;

    return ret;
}

/**
 * @brief 浅拷贝父进程的 fd 表给子进程（fork 用）
 * @param[out] dst 目标 pcb（子进程），fd 表应为空（alloc_new_proc 已 memset 清零）
 * @param[in]  src 源 pcb（父进程）
 * @retval ENO0_NO_ERROR 恒成功
 * @details 逐槽复制指针，父子共享同一批 file_t（不是深拷贝出独立实例）——
 *   两者的读写偏移 f_pos 因此是共享的，这是 POSIX fork 的既定语义。每个非空
 *   槽位对应的 file_t->f_count 递增，配合 proc_fd_close()/proc_fd_close_all()
 *   的引用计数递减，保证底层资源在最后一个引用者关闭前不会被提前释放。
 */
int proc_fd_copy(pcb_t *dst, pcb_t *src)
{
    for (int i = 0; i < NOFILE; i++)
    {
        dst->proc_fds[i] = src->proc_fds[i];
        dst->proc_fd_flags[i] = src->proc_fd_flags[i]; /* FD_CLOEXEC 随 fork 继承，exec 才清 */
        if (src->proc_fds[i])
        {
            atomic_add(&src->proc_fds[i]->f_count, 1);
        }
    }

    return ENO0_NO_ERROR;
}

/**
 * @brief 关闭一个进程 fd 表中所有已打开的文件描述符（exit 用）
 * @param[in] p 要清空 fd 表的 pcb
 * @details 只在槽位非空时才加锁调用 vfs_close()——大多数内核线程/测试 worker
 *   fd 表全空（从不碰文件），不该仅仅为了走一遍空循环就在 do_exit() 早期引入
 *   一个新的睡眠点（VFS 大锁是睡眠信号量，哪怕大概率不阻塞，获取本身仍是一次
 *   潜在的 sched_schedule()/switch_to() 往返）。
 */
void proc_fd_close_all(pcb_t *p)
{
    for (int i = 0; i < NOFILE; i++)
    {
        if (p->proc_fds[i])
        {
            bool need_lock = vfs_file_needs_lock(p->proc_fds[i]);
            if (need_lock)
            {
                vfs_lock();
            }
            vfs_close(p->proc_fds[i]);
            if (need_lock)
            {
                vfs_unlock();
            }
        }
        p->proc_fds[i] = NULL;
        p->proc_fd_flags[i] = 0;
    }
}

/**
 * @brief execve 成功、已确认不会回滚之后调用：关闭所有带 FD_CLOEXEC 的 fd
 * @param[in] p 目标 pcb（正在 execve 的当前进程）
 */
void proc_fd_close_on_exec(pcb_t *p)
{
    for (int i = 0; i < NOFILE; i++)
    {
        if (p->proc_fds[i] && (p->proc_fd_flags[i] & FD_CLOEXEC))
        {
            bool need_lock = vfs_file_needs_lock(p->proc_fds[i]);
            if (need_lock)
            {
                vfs_lock();
            }
            vfs_close(p->proc_fds[i]);
            if (need_lock)
            {
                vfs_unlock();
            }
            p->proc_fds[i] = NULL;
            p->proc_fd_flags[i] = 0;
        }
    }
}

/**
 * @brief 给当前进程装上 stdin/stdout/stderr（fd 0/1/2）
 * @retval ENO0_NO_ERROR   成功
 * @retval ENO1_NOMORE_MEM TTY file 分配失败
 * @details 三个标准 fd 指向同一个 TTY file（输入、输出、错误输出物理上是同一个终端），
 *   引用计数随之为 3。
 */
int proc_install_stdio(void)
{
    file_t *con = tty_open_file();
    if (con == NULL)
    {
        return ENO1_NOMORE_MEM;
    }

    proc_fd_install(0, con);
    atomic_add(&con->f_count, 1);
    proc_fd_install(1, con);
    atomic_add(&con->f_count, 1);
    proc_fd_install(2, con);

    return ENO0_NO_ERROR;
}

/**
 * @brief 获取当前 hart 正在运行的进程
 * @return 当前进程的 pcb 指针
 */
pcb_t *proc_get_current(void)
{
    /* 关中断读：读 hartid 与解引用该 hart 的 cpu_t 之间若被中断走、又在别的 hart 上
     * 被换上，读到的就是另一个 hart 的 current_proc。这里不涉及任何锁，直接用
     * 中断开关的原语即可，不需要走 spinlock 那套 key。 */
    bool key;
    __local_intr_save(key);
    cpu_t *cpu = cpu_get_current();
    pcb_t *proc = cpu->current_proc;
    __local_intr_restore(key);
    return proc;
}
