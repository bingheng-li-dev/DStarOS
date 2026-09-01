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

/* List of all processes. */
struct list_head proc_list;
/* 保护 proc_list 的插入/删除/遍历。sys_kill 要遍历它找目标，而 do_wait 收割 ZOMBIE 时
 * 会 list_del + kfree(pcb)，两者并发就是必现的 use-after-free（slab 会立刻把那块内存
 * 交给别人）。锁序：tty->lock → proc_list_lock → sighand->lock → run_queue.lock，
 * 单向；持有本锁期间不得睡眠、不得取 VFS 大锁。 */
static osslock_t proc_list_lock;
/* Stack of all dealloced pids.In order to alloc these pids again. */
struct list_head pid_stack;
/* Amount of processes. */
uint16_t task_count = 0;

#define USER_STACK_LEN  (16 * PGSIZE)      /* 64 KB，懒分配 */

/* Alloc a new empty pcb(proc). */
static pcb_t *alloc_new_proc(void);
/* Alloc the kernel stack of a proc. */
static int16_t alloc_kernel_stack(pcb_t *pcb);
/* Dealloc the kernel stack of a proc.Attention that it doesn't dealloc the memory pointed by pointers of the pcb!! */
static int16_t dealloc_kernel_stack(pcb_t *pcb);
/* Copy the virtual memory management struct of a proc. */
static int16_t copy_proc_mm(uint32_t clone_flags, pcb_t *pcb);
/* Copy the stack which is up to the param stack(if stack==0, It means to fork a kernel thread). */
static void copy_proc_stk(pcb_t *pcb, uintptr_t stack, intstkf_t *regs);
/* Create task idle. */
static pcb_t *create_first_proc_idle(void);
/* 建一个独立用户地址空间（新 PGD + 复制内核高半段）；失败返回 NULL。不切 satp、不挂 pcb。 */
static mm_t *create_user_mm(void);
/* Find the pcb of a proc by its pid. */
static pcb_t *find_proc_by_pid(int16_t pid);
/* Alloc a unique pid for process. */
static int16_t alloc_pid_map(void);
static void dealloc_pid_map(int16_t pid);
/* Kernel's init process which pid is 1. */
static int16_t init(void);
static void fork_out(void);
#if DEBUG_PROC_CTXSTK
static void print_ctx_stk(ctx_t *ctx) __attribute__((used));
#endif

char *set_proc_name(pcb_t *proc, const char *name)
{
    memset(proc->proc_pname, 0, PNAME_MAX_LENGTH);
    return memcpy(proc->proc_pname, name, sizeof(name));
}

/* Remember to recycle memory of name. */
char *get_proc_name(pcb_t *proc)
{
    char *name = kmalloc(PNAME_MAX_LENGTH);
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->proc_pname, PNAME_MAX_LENGTH);
}

/* @param stack the parent's user stack pointer. if stack==0, It means to fork a kernel thread. */
int16_t do_fork(uint32_t clone_flags, uintptr_t stack, intstkf_t *regs)
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

    /* 信号：继承屏蔽字与全部 handler，但 **挂起集清零**——父进程还没处理完的信号
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

    copy_proc_mm(clone_flags, new_proc);
    copy_proc_stk(new_proc, stack, regs);

    /* 子进程继承父进程的当前工作目录 */
    new_proc->proc_cwd = proc_get_current()->proc_cwd;
    if (new_proc->proc_cwd)
    {
        /* 增加 cwd 目录项的引用计数，防止父进程 chdir 后 dentry 被释放 */
        dentry_get_pub(new_proc->proc_cwd);
    }

    /* 子进程继承父进程的 fd 表：浅拷贝指针 + 每个 file_t 的 f_count++（父子共享打开文件与偏移）*/
    proc_fd_copy(new_proc, proc_get_current());


    int16_t pid = ENO3_NOFREE_PID;
    pid = alloc_pid_map();
    new_proc->proc_pid = pid;

#if DEBUG_PROC_do_fork
    printf("do_fork::pid:%d\n", pid);
#endif

    irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
    list_add(&(new_proc->proc_list_linker), &(proc_list));
    task_count = task_count + 1;
    spinlock_release(&proc_list_lock, plist_key);

    new_proc->proc_parent = proc_get_current();
    list_add_tail(&(new_proc->proc_sibling_linker), &(proc_get_current()->proc_children));

    new_proc->proc_state = RUNNING;
    sched_activate(new_proc);

    return pid;
f1:
    return ENO2_ALLOCPROC_FAILED;
f2:
    kfree(new_proc);
    return ENO1_NOMORE_MEM;
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

void do_exit(int16_t error_code)
{
    exit_common(error_code, 0);
}

void do_exit_signal(int sig)
{
    exit_common(0, (uint8_t)sig);
}

static void exit_common(int16_t error_code, uint8_t sig)
{
    pcb_t *curr = proc_get_current();

    proc_fd_close_all(curr);

    /* 归还工作目录的引用。只在真的 chdir 过时才取大锁，理由同 proc_fd_close_all()：
     * 内核线程的 proc_cwd 绝大多数是 NULL，不值得为此在退出路径上白添一个睡眠点。
     * 必须赶在置 ZOMBIE 之前做完——父进程一被唤醒就可能把这个 pcb 收割掉。 */
    if (curr->proc_cwd)
    {
        dentry_t *cwd = curr->proc_cwd;
        curr->proc_cwd = NULL;
        vfs_lock();
        dentry_put_pub(cwd);
        vfs_unlock();
    }

    /* 孤儿过继给 init（pid 恒为 1：系统里第一个 do_fork 出来的进程）；
     * 有孤儿就唤醒 init，让它有机会发现并收割这些可能已经是 ZOMBIE 的孤儿 */
    pcb_t *init_proc = find_proc_by_pid(1);
    bool has_orphan = false;
    struct list_head *pos, *tmp;
    list_for_each_safe(pos, tmp, &curr->proc_children)
    {
        pcb_t *child = list_entry(pos, pcb_t, proc_sibling_linker);
        child->proc_parent = init_proc;
        list_del(&child->proc_sibling_linker);
        list_add_tail(&child->proc_sibling_linker, &init_proc->proc_children);
        has_orphan = true;
    }
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
    curr->proc_state = ZOMBIE;
    /* pid 不在这里回收——ZOMBIE 期间 pid 必须继续"占用"，
     * 否则两次退出之间创建的新进程可能撞上同一个 pid。
     * 真正的回收在 do_wait() 收割时才做 */
    if (curr->proc_parent)
    {
        /* SIGCHLD 默认动作是忽略，signal_send 的忽略优化会把它直接丢掉，
         * 所以对没装 handler 的父进程这一句等于零成本 */
        signal_send(curr->proc_parent, SIGCHLD);
        wakeup(curr->proc_parent);
    }
    
    sched_schedule();

    panic("Zombie task resumed, should never happen\n");
}

/** 
 * @param pid    -1 = 等任意子进程（本阶段只支持 -1）
 * @param status 出参：POSIX 编码的退出状态（(exit_code & 0xff) << 8）；NULL 表示不关心
 * @retval >0    被收割子进程的 pid
 * @retval <0    ENO*（ENO17_NO_CHILD：无子进程） */
int16_t do_wait(int16_t pid, int *status)
{
    pcb_t *cur = proc_get_current();
    (void)pid;

    while (1)
    {
        /* 先置睡眠状态，再检查条件：若子进程恰好在这两步之间 do_exit()，
         * 它的 wakeup(cur) 会把状态改回 RUNNING，本轮循环末尾的 sched_schedule()
         * 会因为 curr 仍是 RUNNING 而重新入队、立刻continue，不会睡死过去 */
        cur->proc_state = INTERRUPTIBLE;

        bool has_child = false;
        struct list_head *pos;
        list_for_each(pos, &cur->proc_children)
        {
            pcb_t *child = list_entry(pos, pcb_t, proc_sibling_linker);
            has_child = true;
            if (child->proc_state == ZOMBIE)
            {
                cur->proc_state = RUNNING;

                /* 子进程在 do_exit 里的顺序是"先置 ZOMBIE + wakeup(父进程)，再
                 * sched_schedule()"，所以父进程被唤醒时，子进程很可能还站在自己的
                 * 内核栈上往 switch_to 走（接上 IPI 之后父进程会被立刻唤醒到另一个
                 * hart 上）。必须等它真正把上下文保存完、
                 * 彻底离开 CPU，才能回收它的内核栈和 PCB——否则就是在它脚下把正在
                 * 使用的栈释放掉，典型表现是内核线程（proc_mm 为 NULL）在随机地址
                 * 上页错误。proc_on_cpu 由 sched_finish_switch() 在 switch_to 完成后
                 * 清零，单核下父进程能跑起来就说明子进程早已让出，循环不会真的转。 */
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

                /* 收割：从父的 children、全局 proc_list 摘掉，释放它自己没法释放的
                 * 内核栈与 PCB（还站在上面跑的时候不能自己拆），回收 PID */
                list_del(&child->proc_sibling_linker);
                irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
                list_del(&child->proc_list_linker);
                task_count -= 1;
                spinlock_release(&proc_list_lock, plist_key);
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
        }

        if (!has_child)
        {
            cur->proc_state = RUNNING;
            return ENO17_NO_CHILD;
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

/**
 * @brief 用 path 指向的 ELF 替换当前进程的地址空间（execve 语义：换脑不换壳）
 * @param[in,out] sp   当前 syscall 的 trap 帧；成功时被改写为"进入新程序"的帧
 * @param[in]     path 用户空间的程序路径字符串
 * @retval ENO0_NO_ERROR 成功——trap 帧已指向新程序，返回后 sret 即进入新程序，旧程序视角看不到此返回值
 * @retval <0 失败（负 ENO*）——旧地址空间原封不动，exec 失败不致命，返回值传回旧程序
 * @details 保留 PCB / PID / 父子关系 / fd 表 / cwd，只把地址空间整个换掉。顺序极其关键：
 *   1. **先**把 path 从用户空间拷进内核（切 satp 后用户指针失效）；
 *   2. **先**把整个 ELF 读进内核堆（内核偏移映射，切 satp 后仍可达）；
 *   3. 建新 mm、切到新地址空间（旧 mm 先留着，加载失败要回滚）；
 *   4. elf_load 到新 mm；失败则切回旧 mm、销毁半成品新 mm、返回错误；
 *   5. 成功后才销毁旧 mm（此刻已不站在它的页表上），随即关闭带 FD_CLOEXEC 的 fd；
 *   6. 建全新用户栈 VMA；fd 表其余部分原样保留；
 *   7. 改写 trap 帧（sepc=入口、sp=新栈顶、清通用寄存器），走正常 syscall 返回路径进入新程序。
 * @note 不做 argv/envp（无 argc 的 _start）；未来铺 argv 时需让成功路径跳过 trap.c 对 a0 的写回。
 * @note FD_CLOEXEC 的 fd 在第 5 步（销毁旧 mm 之后）才关闭，而不是一进函数就关——第 1-4 步
 *   随时可能失败并回滚到旧程序继续执行，那种情况下 fd 表必须原封不动（POSIX 语义：
 *   execve 失败等价于没发生过）；只有确认新程序已经站稳（旧 mm 已销毁、没有回头路）之后，
 *   关闭 CLOEXEC fd 才是安全的。
 */
int do_exec(intstkf_t *sp, const char *path)
{
    pcb_t *cur = proc_get_current();

    /* 1) 路径来自用户空间：切 satp 前拷进内核缓冲 */
    char kpath[VFS_PATH_MAX];
    long path_len = strncpy_from_user(kpath, path, sizeof(kpath));
    if (path_len < 0)
    {
        return (int)path_len;
    }

    /* 2) 打开并把整个 ELF 读进内核堆 */
    vfs_lock();
    file_t *f = vfs_open(kpath, O_RDONLY);
    if (f == NULL)
    {
        vfs_unlock();
        return ENO5_NOSUCH_ENTRY;
    }
    off_t size = vfs_lseek(f, 0, SEEK_END);
    vfs_lseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        vfs_close(f);
        vfs_unlock();
        return ENO6_INVAL_PARAM;
    }
    unsigned char *img = kmalloc((size_t)size);
    if (img == NULL)
    {
        vfs_close(f);
        vfs_unlock();
        return ENO1_NOMORE_MEM;
    }
    ssize_t rd = vfs_read(f, img, (size_t)size);
    vfs_close(f);
    vfs_unlock();
    if (rd != (ssize_t)size)
    {
        kfree(img);
        return ENO6_INVAL_PARAM;
    }

    /* 3) 建新地址空间并切过去（旧 mm 先留着）*/
    mm_t *old_mm = cur->proc_mm;
    mm_t *new_mm = create_user_mm();
    if (new_mm == NULL)
    {
        kfree(img);
        return ENO1_NOMORE_MEM; /* 旧地址空间原封不动 */
    }
    cur->proc_mm = new_mm;
    cur->proc_context.satp = SATPMODE_RV39 | new_mm->pgd_ppn;
    write_csr(satp, cur->proc_context.satp);
    tlb_flush_all();

    /* 4) 解析 ELF 到新地址空间 */
    virAddr_t entry;
    int ret = elf_load(new_mm, img, (uint64_t)size, &entry);
    kfree(img);
    if (ret != ENO0_NO_ERROR)
    {
        /* 加载失败：切回旧地址空间、销毁半成品新 mm，返回错误（exec 失败不致命）*/
        cur->proc_mm = old_mm;
        cur->proc_context.satp = SATPMODE_RV39 | old_mm->pgd_ppn;
        write_csr(satp, cur->proc_context.satp);
        tlb_flush_all();
        vmm_mm_destroy(new_mm);
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

    /* 6) 全新用户栈 VMA（懒分配）；fd 表其余部分原样保留 */
    vma_t *stk = vmm_vma_create(USER_STACK_TOP - USER_STACK_LEN, USER_STACK_TOP, VMP_R | VMP_W);
    vmm_vma_insert(new_mm, stk);

    /* 7) 改写当前 trap 帧：sret 直接进入新程序（复用 syscall 返回路径，不另起 enter_user_mode）*/
    memset(sp, 0, sizeof(intstkf_t));
    sp->sepc    = entry;
    sp->x2_sp   = USER_STACK_TOP;
    sp->x4_tp   = cpu_get_core_id();
    sp->sstatus = (read_csr(sstatus) & ~SSTATUS_SPP) | SSTATUS_SPIE | SSTATUS_SUM;

    return ENO0_NO_ERROR;
}

int16_t create_kernel_thread_by_fork(void *func(void *), void *args, uint32_t clone_flags)
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

/**
 * @brief 进程子系统的全局结构初始化，只由 hart0 调用一次
 * @note **必须在启动 hart1 之前调用**。从 proc_init() 里拆出来的理由：hart1 一上电就会
 *   跑自己的 proc_init()，那里会碰 task_count（受 proc_list_lock 保护）。若把
 *   spinlock_init 留在 proc_init() 里，就成了"hart1 可能先拿锁、hart0 随后把这把锁
 *   重新初始化"——正持有的锁被清零，之后谁都进不去。
 */
void proc_early_init(void)
{
    spinlock_init(&proc_list_lock);
    INIT_LIST_HEAD(&proc_list);
    INIT_LIST_HEAD(&pid_stack);
}

void proc_init(void)
{
    /* 每个 hart 各自的 idle 任务则必须每 hart 都建 */
    pcb_t *idle = create_first_proc_idle();
    if (idle == NULL)
    {
        panic("Failed to alloc idle proc!!\n");
    }
    cpu_get_current()->idle_proc = idle;
    sched_set_current(idle);

#if DEBUG_PROC_proc_init
    printf("%s::TaskCurrent->need_resched:%d TaskIdle->need_resched %d\n", 
        __FUNCTION__, proc_get_current()->need_resched, idle->need_resched);
    printf("%s::TaskCurrent addr:%lx TaskIdle addr %lx\n", 
        __FUNCTION__, (intptr_t)proc_get_current(), (intptr_t)idle);
#endif

    if (cpu_get_core_id() == 0)
    {
        int16_t id_init = create_kernel_thread_by_fork((void *)init, NULL, 0);
        pcb_t *pcb_init = find_proc_by_pid(id_init);
        const char *name = "init";
        set_proc_name(pcb_init, name);

#if DEBUG_PROC_proc_init
        printf("%s::pcb_init pid:%d\n", __FUNCTION__, id_init);
        printf("%s::pcb_init addr:%lx\n", __FUNCTION__, (intptr_t)pcb_init);
#endif

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
        memset(pcb->proc_pname, 0, PNAME_MAX_LENGTH);
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

        pcb->proc_wake_tick = 0;
        INIT_LIST_HEAD(&(pcb->proc_timer_linker));
        
#if DEBUG_PROC_allocNewProc
        printf("alloc_new_proc::new pcb addr:%lx,sizeof(pcb_t):%ld\n", (intptr_t)pcb, sizeof(pcb_t));
#endif
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

    /* 用户线程fork */
    mm_t *mm = vmm_mm_create();
    if (!mm)
    {
        panic("Failed to alloc new mm for child process!\n");
    }

    /* 必须先建立独立 PGD 并设好 mm->pgd_ppn，vmm_mm_copy 才能向正确的页表写 PTE */
    pframe_t *new_pgd_frame = slab_alloc_page_retry();
    if (!new_pgd_frame)
    {
        panic("Failed to alloc new page for child process PGD!\n");
    }
    memset((void *)convert_pframe2kva(new_pgd_frame), 0, PGSIZE);
    new_pgd_frame->reference += 1;
    mm->pgd_ppn = convert_pframe2ppn(new_pgd_frame);
    /* 复制内核半段（PGD[256..511]），使子进程能访问内核地址空间 */
    memcpy(
        (void *)pa_to_kva(convert_ppn2pa(mm->pgd_ppn)) + sizeof(pte_t) * 256,
        (void *)pa_to_kva(convert_ppn2pa(vmm_kernel_pgd_ppn)) + sizeof(pte_t) * 256,
        sizeof(pte_t) * 256
    );

    vmm_mm_copy(mm, proc_get_current()->proc_mm);

    pcb->proc_mm = mm;
    pcb->proc_context.satp = SATPMODE_RV39 | mm->pgd_ppn;

    return ENO0_NO_ERROR;
}

static void copy_proc_stk(pcb_t *pcb, uintptr_t stack, intstkf_t *regs)
{
    pcb->proc_int_stack = (intstkf_t *)(pcb->kernel_stack + KERNRL_STKSIZE - sizeof(intstkf_t));
    *(pcb->proc_int_stack) = *(regs);
    /* For child process,"fork" returns 0. */
    pcb->proc_int_stack->x10_a0 = 0;
    pcb->proc_int_stack->x2_sp = (stack == 0) ? (uintptr_t)pcb->proc_int_stack : stack;
    pcb->proc_context.x1_ra = (uint64_t)fork_out;
    pcb->proc_context.x2_sp = (uint64_t)pcb->proc_int_stack;
}

static pcb_t *create_first_proc_idle(void)
{
    extern uintptr_t boot_stack_top1;
    extern uintptr_t boot_stack_top2;

    pcb_t *idle = alloc_new_proc();
    if (idle != NULL)
    {
        idle->proc_pid = 0;
        /* alloc_new_proc 默认把所有任务挂 &fair_sched_class，idle 单独覆盖成 &idle_sched_class */
        idle->proc_sched_class = &idle_sched_class;
        /* 每个 hart 用自己在 startup.S 里的启动栈作内核栈：core0→boot_stack_top1，core1→boot_stack_top2 */
        idle->kernel_stack = (phyAddr_t)(cpu_get_core_id() == 0 ? boot_stack_top1 : boot_stack_top2);
        idle->proc_state = RUNNING;
        idle->need_resched = true;
        idle->proc_cwd = NULL;  /* idle 进程使用 VFS 根目录 */
        const char *name = "idle";
        set_proc_name(idle, name);
        /* 两个 hart 会并发建各自的 idle，task_count 的自增必须串行化 */
        irq_key_t plist_key = spinlock_acquire(&proc_list_lock);
        task_count = task_count + 1;
        spinlock_release(&proc_list_lock, plist_key);
    }
#if DEBUG_PROC_createFirstProcIdle
    printf("create_first_proc_idle::idle->need_resched:%d\n", idle->need_resched);
#endif
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
#if DEBUG_PROC_findProcByPid
                printf("find_proc_by_pid::currentPcb->proc_pid:%d,currentPcb->proc_pname:%s\n", currentPcb->proc_pid, currentPcb->proc_pname);
#endif
                spinlock_release(&proc_list_lock, plist_key);
                return currentPcb;
            }
        }
        spinlock_release(&proc_list_lock, plist_key);
    }
    return NULL;
}

/* 按 pid 查找 pcb 的公开包装，供 do_wait 之外的模块（如调度回归测试）使用。 */
pcb_t *proc_find_by_pid(int16_t pid)
{
    return find_proc_by_pid(pid);
}

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

static int16_t alloc_pid_map(void)
{
    static uint16_t last_alloc = 0;

    /* 优先复用已归还的 pid：FIFO（摘链表头，dealloc_pid_map 从链表尾插入）——
     * 最早归还的先被复用，尽量拖延"刚死的进程 pid 立刻被新进程占用"这个复用陷阱，
     * 不然以后 wait/kill 之类按 pid 操作的功能可能误伤到复用了旧 pid 的新进程 */
    if (!list_empty(&pid_stack))
    {
        struct list_head *head = pid_stack.next;
        pids_t *cur = list_entry(head, pids_t, pid_stk_linker);
        list_del_init(head);
        int16_t ret = cur->pid;
        kfree(cur);
        return ret;
    }

    /* 没有可复用的：发一个全新号；用满 [1, PID_MAX_VALUE] 后从头回绕，
     * 每个候选号都用 find_proc_by_pid 确认真的空闲，避免跟存活进程撞号 */
    for (uint16_t tried = 0; tried < PID_MAX_VALUE; tried++)
    {
        last_alloc = (last_alloc % PID_MAX_VALUE) + 1;
        if (find_proc_by_pid(last_alloc) == NULL)
        {
            return (int16_t)last_alloc;
        }
    }

    return ENO3_NOFREE_PID; /* 整个 pid 空间都被占满 */
}

/* A "pids_t" will be alloced when a pid were being dealloced. */
static void dealloc_pid_map(int16_t pid)
{
    /* Alloc a new "pids_t". */
    pids_t *cur = (pids_t *)kmalloc(sizeof(pids_t));
    cur->pid = pid;
    /* 插到尾部，配合 alloc_pid_map 从头摘，构成 FIFO */
    list_add_tail(&(cur->pid_stk_linker), &pid_stack);
}

void idle(void)
{
    while (1)
    {
#if DEBUG_PROC_idle
        pcb_t *cur = proc_get_current();
        pcb_t *my_idle = cpu_get_current()->idle_proc;
        printf("%s::TaskCurrent->need_resched:%d TaskIdle->need_resched %d\n",
            __FUNCTION__, cur->need_resched, my_idle->need_resched);
        printf("%s::TaskCurrent->proc_pname:%s TaskIdle->proc_pname %s\n",
            __FUNCTION__, cur->proc_pname, my_idle->proc_pname);
        printf("%s::TaskCurrent->proc_pid:%d TaskIdle->proc_pid %d\n",
            __FUNCTION__, cur->proc_pid, my_idle->proc_pid);
#endif

        /* 每轮都尝试调度，不再靠 need_resched 门槛——本 hart 的 need_resched 只有
         * "别的任务在本 hart 上跑时被抢占"才会置位，本 hart 自己空转时永远不会有人
         * 帮它置这个标志，靠它当门槛会导致这个 hart 永久看不到共享就绪队列里别的
         * hart 刚放进去的任务。sched_schedule() 在确实没活干时会退化成挑回自己的
         * idle_proc，开销很小。 */
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

extern const unsigned char user_elf[];
extern const unsigned long user_elf_len;

/**
 * @brief 建一个独立的用户地址空间（新 PGD + 复制内核高半段）
 * @return 新 mm；失败返回 NULL
 * @details 复用 copy_proc_mm 用户分支的套路：分配根页表帧、清零、复制内核半段 PGD[256..511]，
 *   使新地址空间也能访问内核。**不切 satp、不挂到任何 pcb**——由调用方决定何时切换
 *   （run_user_program 首次进入 / do_exec 换脑）。
 */
static mm_t *create_user_mm(void)
{
    mm_t *mm = vmm_mm_create();
    if (mm == NULL)
    {
        return NULL;
    }
    pframe_t *pgd = slab_alloc_page_retry();
    if (pgd == NULL)
    {
        return NULL; /* OOM 极端边界：此处不回收 mm（系统已濒临耗尽），可接受 */
    }
    memset((void *)convert_pframe2kva(pgd), 0, PGSIZE);
    pgd->reference += 1;
    mm->pgd_ppn = convert_pframe2ppn(pgd);
    memcpy((void *)pa_to_kva(convert_ppn2pa(mm->pgd_ppn)) + sizeof(pte_t) * 256,
           (void *)pa_to_kva(convert_ppn2pa(vmm_kernel_pgd_ppn)) + sizeof(pte_t) * 256,
           sizeof(pte_t) * 256);

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
 * @brief 建独立用户地址空间、加载给定 ELF 字节数组并进入 U 态
 * @details 供 `run_first_user_program`/`run_fork_wait_test_program` 共用，两者仅嵌入的 ELF
 *   字节数组不同（`user_elf` / `user_fork_wait_elf`），其余建 mm/切地址空间/建用户栈的流程一致。
 * @param[in] elf     嵌入式 ELF64 字节数组
 * @param[in] elf_len 数组长度
 * @note noreturn：`enter_user_mode` 内部 `sret` 进入 U 态，不会返回
 */
static void run_user_program(const unsigned char *elf, unsigned long elf_len)
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

    /* 3) 解析 ELF：按 PT_LOAD 段建 VMA、映射、拷贝内容，得到程序入口地址 */
    virAddr_t entry;
    int ret = elf_load(mm, elf, elf_len, &entry);
    if (ret != ENO0_NO_ERROR)
    {
        panic("run_user_program: elf_load failed, ret=%d", ret);
    }

    /* 4) 用户栈 VMA（懒分配，首次访问由 page fault 落实） */
    vma_t *stk = vmm_vma_create(USER_STACK_TOP - USER_STACK_LEN, USER_STACK_TOP, VMP_R | VMP_W);
    vmm_vma_insert(mm, stk);

    /* 5) 装 stdin/stdout/stderr（fd 0/1/2）；fork 出的子进程由 do_fork 的 proc_fd_copy 继承 */
    proc_install_stdio();

    /* 6) 信号处理表：init 变身用户进程之后才需要，fork 出的子进程由 do_fork 继承。
     * 顺带把它设成 TTY 的前台进程组——否则 ^C 打给的还是初值 1（init 的 pgid），
     * 而 init 是内核线程，收不到信号，按下去什么也不会发生。真实系统里这一步由
     * shell 的 tcsetpgrp 接管，在那之前"第一个用户进程就是前台作业"是对的。 */
    proc_signal_init_user(cur);
    tty_set_foreground_pgid(cur->proc_pgid);

    /* 7) 进入 U 态 */
    enter_user_mode(entry, USER_STACK_TOP);
}

#if !DEBUG_EXEC_TEST && !DEBUG_FILE_TEST && !DEBUG_PIPE_TEST && !DEBUG_TTY_TEST && !DEBUG_MEM_TEST && !DEBUG_SIGNAL_TEST
static void run_first_user_program(void)
{
    run_user_program(user_elf, user_elf_len);
}
#endif

#if DEBUG_FORK_WAIT_TEST
static void run_fork_wait_test_program(void)
{
    extern const unsigned char user_fork_wait_elf[];
    extern const unsigned long user_fork_wait_elf_len;
    run_user_program(user_fork_wait_elf, user_fork_wait_elf_len);
}
#endif

#if DEBUG_EXEC_TEST || DEBUG_FILE_TEST
/* 把嵌入的 hello ELF 写进 ramdisk 的 "/hello"，给 exectest 的 execve 一个可加载的目标；
 * 顺带验证 VFS 写盘（这是 rootfs 上第一个真实写入的文件）。在 init（内核上下文）里调用。
 * filetest 也依赖它：那里的 getdents64 用例要在根目录列表里看到 "hello" 这一项。 */
static void seed_exec_target(void)
{
    file_t *f = vfs_open("/hello", O_CREAT | O_WRONLY | O_TRUNC);
    if (f == NULL)
    {
        printf("[init] seed /hello: vfs_open failed\n");
        return;
    }
    ssize_t w = vfs_write(f, user_elf, user_elf_len);
    vfs_close(f);
    printf("[init] seed /hello: wrote %ld bytes\n", (long)w);
}
#endif

#if DEBUG_EXEC_TEST
static void run_exectest_program(void)
{
    extern const unsigned char user_exectest_elf[];
    extern const unsigned long user_exectest_elf_len;
    run_user_program(user_exectest_elf, user_exectest_elf_len);
}
#endif

#if DEBUG_FILE_TEST
static void run_filetest_program(void)
{
    extern const unsigned char user_filetest_elf[];
    extern const unsigned long user_filetest_elf_len;
    run_user_program(user_filetest_elf, user_filetest_elf_len);
}
#endif

#if DEBUG_PIPE_TEST
static void run_pipetest_program(void)
{
    extern const unsigned char user_pipetest_elf[];
    extern const unsigned long user_pipetest_elf_len;
    run_user_program(user_pipetest_elf, user_pipetest_elf_len);
}
#endif

#if DEBUG_TTY_TEST
static void run_ttytest_program(void)
{
    extern const unsigned char user_ttytest_elf[];
    extern const unsigned long user_ttytest_elf_len;
    run_user_program(user_ttytest_elf, user_ttytest_elf_len);
}
#endif

#if DEBUG_MEM_TEST
static void run_memtest_program(void)
{
    extern const unsigned char user_memtest_elf[];
    extern const unsigned long user_memtest_elf_len;
    run_user_program(user_memtest_elf, user_memtest_elf_len);
}
#endif

#if DEBUG_SIGNAL_TEST
static void run_sigtest_program(void)
{
    extern const unsigned char user_sigtest_elf[];
    extern const unsigned long user_sigtest_elf_len;
    run_user_program(user_sigtest_elf, user_sigtest_elf_len);
}
#endif

static int16_t init(void)
{
    printf("%s::Hello! I'm the init process!!\n", __FUNCTION__);

#if DEBUG_PROC_init
    /* 验证内核线程 satp 正确：switch_to 切换后，通过 KVA 读写新分配的物理帧。
     * 若 satp 被 switch_to 写成 0（BARE 模式），此处访问高位 VA 会触发 access fault。 */
    pframe_t *t_frame = alloc_page();
    if (!t_frame)
    {
        panic("init test: alloc_page returned NULL");
    }
    volatile uint64_t *tp = (volatile uint64_t *)convert_pframe2kva(t_frame);
    *tp = 0xabcd1234ef567890UL;
    if (*tp != 0xabcd1234ef567890UL)
    {
        panic("init test: kernel thread KVA FAILED - satp incorrect after switch_to");
    }
    dealloc(t_frame);
    printf("[init] kernel thread KVA after switch_to: PASS\n");
#endif

#if DEBUG_SLAB_TEST
    /* slab 分配器自检：跑完直接关机，不启动用户程序。测试代码在 src/debug/slab_test.c。 */
    {
        extern void run_slab_tests(void);
        run_slab_tests();
        sbi_shutdown();
    }
#endif

#if DEBUG_VFS_TEST
    /* VFS/FatFS 回归：跑完直接关机，不启动用户程序。测试代码在 src/debug/vfs_fatfs_test.c。 */
    {
        extern void vfs_test(void);
        vfs_test();
        sbi_shutdown();
    }
#endif

#if DEBUG_DCACHE_TEST
    /* 目录项缓存自检：跑完直接关机，不启动用户程序。测试代码在 src/debug/dcache_test.c。 */
    {
        extern void run_dcache_tests(void);
        run_dcache_tests();
        sbi_shutdown();
    }
#endif

#if DEBUG_SCHED_TEST
    /* 调度器/同步回归测试：以 init（正规调度任务）为驱动，fork 若干 worker 并收割，
     * 端到端触发 CFS/RT/idle 三类、sched_schedule、sleep/wakeup、信号量、do_fork/exit/wait。
     * 跑完直接关机，不再启动用户程序。测试代码在 src/debug 下的 sched_test.c 等文件。 */
    {
        extern void run_sched_tests(void);
        run_sched_tests();
        printf("[init] scheduler tests done, shutting down\n");
        sbi_shutdown();
    }
#endif

#if DEBUG_FORK_WAIT_TEST
    /* 验证 sys_clone/sys_wait4：见 user/fork_wait.c，跑完串口应看到 "child: hi" 和
     * "parent: reaped pid=<N> exitcode=42"，随后这个测试进程自己 exit(0) 被 init 收割。 */
    {
        int16_t fw_pid = create_kernel_thread_by_fork((void *)run_fork_wait_test_program, NULL, 0);
        if (fw_pid < 0)
        {
            panic("Failed to fork fork_wait test program thread!\n");
        }
    }
#endif

#if DEBUG_EXEC_TEST
    /* 验证 dup + execve：先在 ramdisk 塞好 /hello，再跑 exectest（不跑默认用户程序）*/
    seed_exec_target();
    int16_t pid = create_kernel_thread_by_fork((void *)run_exectest_program, NULL, 0);
#elif DEBUG_FILE_TEST
    /* 验证 POSIX 文件 syscall：同样先塞好 /hello（filetest 的 getdents64
     * 用例要在根目录列表里看到它），再跑 filetest（不跑默认用户程序）*/
    seed_exec_target();
    int16_t pid = create_kernel_thread_by_fork((void *)run_filetest_program, NULL, 0);
#elif DEBUG_PIPE_TEST
    /* 验证管道 syscall：pipetest 不依赖 /hello（不做 execve），
     * 不需要 seed_exec_target，直接跑（不跑 exectest/filetest/默认用户程序）*/
    int16_t pid = create_kernel_thread_by_fork((void *)run_pipetest_program, NULL, 0);
#elif DEBUG_TTY_TEST
    /* 验证 TTY 行规范层：ttytest 不依赖 /hello，不需要 seed_exec_target，
     * 直接跑（不跑 exectest/filetest/pipetest/默认用户程序）*/
    int16_t pid = create_kernel_thread_by_fork((void *)run_ttytest_program, NULL, 0);
#elif DEBUG_MEM_TEST
    /* 验证内存管理 syscall：memtest 不依赖 /hello，不需要 seed_exec_target，
     * 直接跑（不跑 exectest/filetest/pipetest/ttytest/默认用户程序）*/
    int16_t pid = create_kernel_thread_by_fork((void *)run_memtest_program, NULL, 0);
#elif DEBUG_SIGNAL_TEST
    /* 验证信号：sigtest 不依赖 /hello，不需要 seed_exec_target，
     * 直接跑（不跑 exectest/filetest/pipetest/ttytest/memtest/默认用户程序）*/
    int16_t pid = create_kernel_thread_by_fork((void *)run_sigtest_program, NULL, 0);
#else
    int16_t pid = create_kernel_thread_by_fork((void *)run_first_user_program, NULL, 0);
#endif
    if (pid < 0)
    {
        panic("Failed to fork user program thread!\n");
    }

    /* 永久收割循环：孤儿最终都会过继到这里，没有这个循环孤儿僵尸会永久堆积 */
    while (1)
    {
        int status;
        int16_t cpid = do_wait(-1, &status);
        if (cpid == ENO24_RESTARTSYS)
        {
            /* init 是内核线程（proc_sighand == NULL），走不到这里；留一手防止
             * 以后 init 变成用户进程时"被信号打断"被误判成"没有子进程了"而关机 */
            continue;
        }
        if (cpid > 0)
        {
            printf("[init] reaped pid=%d status=%d\n", cpid, (status >> 8) & 0xff);
        }
        else
        {
            // @TODO sbi_shutdown测试用
            printf("[init] no more children, shutting down\n");
#if DEBUG_MEM_TEST || DEBUG_FILE_TEST
            /* 压力用例跑完之后核对各 cache 的 nr_inuse 是否回到基线（vma_cache 尤其）*/
            slab_dump_stats();
            vfs_dcache_stats();
            printf("[init] FreeList.fnsize=%d\n", FreeList.fnsize);
#endif
            sbi_shutdown();
        }
    }
}

static void fork_out(void)
{
    extern void fork_out_asm(intstkf_t * regs);
    /* current_proc 已由 sched_schedule() 在调 switch_to() 之前经 sched_set_current()设好 */

    /* 本执行流第一次被 switch_to() 换上：调用方 sched_schedule() 在 switch_to()
     * 之前 spinlock_acquire(&run_queue.lock) 拿了锁，按"接力"约定应由被换上的执行流
     * 自己补上这次放锁（sched_schedule() 里 switch_to 之后的 sched_finish_switch()
     * 配对，这里是同一约定在"从未被调度过的新执行流"这一分支上的对应写法）。
     * 漏掉会让 run_queue.lock 永远不被释放，整个调度器死锁。
     * 放锁用的中断 key 取自本任务的 proc_rq_key，alloc_new_proc 已把它初始化成 true。 */
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
 * @details
 *   此函数复用 fork_out_asm → trap_return → sret 这条已有的"从 trap 帧恢复并返回"路径，
 *   在内核栈顶伪造一个 trap 帧，使硬件以为这是一次正常的 trap 返回，从而以 U 态身份
 *   跳入用户入口。具体步骤：
 *
 *   1. 在内核栈顶（kernel_stack + KERNRL_STKSIZE）向下划出 sizeof(intstkf_t) 空间，
 *      清零后填写关键字段：
 *        - sepc    = entry        （sret 后 PC 跳至用户入口）
 *        - x2_sp   = ustack       （用户栈顶）
 *        - sstatus = SPP=0        （sret 返回 U 态）
 *                  | SPIE=1       （返回后恢复中断使能）
 *                  | SUM=1        （内核全程可访问用户页，与 trap_init 保持一致）
 *        - x4_tp   = 当前 tp      （维持 core id，单核足够；SMP 下需在 trap_entry 重载）
 *   2. 写 sscratch = 内核栈顶，建立"下次从 U 态 trap 进来时切回内核栈"的不变式
 *      （trap_entry 用 csrrw sp, sscratch, sp 实现栈切换）。
 *   3. 调用 fork_out_asm(f)：将 sp 设为伪造帧地址，跳入 trap_return，
 *      恢复所有寄存器后执行 sret，进入 U 态。
 *
 * @note 此函数不返回（标注 __attribute__((noreturn))）。
 *   调用前须确保：
 *     - 当前进程的 proc_mm 已挂载用户地址空间且 satp 已切换；
 *     - entry 所在代码页和 ustack 所在栈 VMA 已就绪（可为懒分配，首次访问触发 page fault）；
 *     - trap_init 已置 sstatus.SUM=1，内核可直接读写用户页。
 */
void enter_user_mode(virAddr_t entry, virAddr_t ustack)
{
    extern void fork_out_asm(intstkf_t *regs) __attribute__((noreturn));

    pcb_t *cur = proc_get_current();
    /* 由高地址向低地址开辟帧空间，不会覆盖原有数据，因为该函数noreturn，原栈空间数据已无用 */
    intstkf_t *f = (intstkf_t *)(cur->kernel_stack + KERNRL_STKSIZE - sizeof(intstkf_t));

    memset(f, 0, sizeof(intstkf_t));
    f->sepc    = entry;
    f->x2_sp   = ustack;
    f->x4_tp   = cpu_get_core_id();
    f->sstatus = (read_csr(sstatus) & ~SSTATUS_SPP) | SSTATUS_SPIE | SSTATUS_SUM;
    write_csr(sscratch, (uintptr_t)(cur->kernel_stack + KERNRL_STKSIZE));
    fork_out_asm(f);
}

/**
 * @brief 在当前进程的 fd 表中找最小可用的文件描述符下标
 * @return 成功返回 [0, NOFILE) 内的下标；fd 表已满返回 ENO18_TOO_MANY_FILES
 * @note 只负责挑号，不写入 fd 表——真正把 file_t 装进去是 proc_fd_install() 的职责，
 *   两步拆分参照 Linux get_unused_fd()/fd_install()。
 * @todo 添加信号机制以后，若信号处理函数在 alloc 与 install 之间重入本进程的
 *   fd 分配路径，会拿到重复的 fd 号；当前无信号机制，不构成问题。
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
            src->proc_fds[i]->f_count++;
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
 * @retval ENO1_NOMORE_MEM console file 分配失败
 * @details 三个标准 fd 共享同一个内核虚构的 console 设备 file（见 console_open_file）——
 *   终端场景下输入/输出/错误输出物理上就是同一个终端，故三个 fd 指向同一 file_t，
 *   引用计数随之为 3。将来有 /dev/console 设备节点后，改成 open 它即可。
 */
int proc_install_stdio(void)
{
    file_t *con = console_open_file();
    if (con == NULL)
    {
        return ENO1_NOMORE_MEM;
    }

    proc_fd_install(0, con);
    con->f_count++;
    proc_fd_install(1, con);
    con->f_count++;
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
