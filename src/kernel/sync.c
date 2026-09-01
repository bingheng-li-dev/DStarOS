#include "sync.h"
#include "dassert.h"
#include "sched.h"
#include "proc.h"
#include "containerof.h"

/* 中断状态由调用者持有，不再有 per-CPU 的嵌套计数器 + 状态槽。
 *
 * 【为什么改】原先的写法是 xv6/ucore 那一路：`cpu->irq_disable_nesting` 计数，
 * 计数 0→1 时把"进来之前中断是开是关"存进 `cpu->intr_disable_state`，1→0 时照它恢复。
 * 问题在于 acquire 与 release **不保证由同一条执行流完成**——`sched_schedule()` 的
 * run_queue.lock 就是接力的：一条流 acquire、switch_to 之后由被换上的那条流 release，
 * 而任务再次被换上时可能已经在另一个 hart。状态存在"CPU"上，配对关系就与执行流脱钩了。
 * 2026-08-31 实测到过 `nesting == 0` 但 `sstatus.SIE == 0`：没人持锁，中断却永久关着，
 * 时钟随之停摆、睡眠任务再也醒不来，表现为整机静默卡死。
 *
 * 【怎么改】跟 Linux 的 `spin_lock_irqsave(lock, flags)` 与 Zephyr 的
 * `k_spin_lock()` 返回 key 一致：**把状态交还给调用者**。局部变量在内核栈上，
 * 内核栈随任务走，任务迁移到别的 hart 也不会配错；调度器那条接力路径则把 key
 * 存进 pcb（`proc_rq_key`），同样是"随任务走"。
 *
 * 嵌套计数器也一并删掉了：内层 acquire 拿到的 key 就是"进来时已经关着"（false），
 * 它的 release 什么都不做，中断只在最外层那次 release 时才真正打开——
 * 计数的效果由 key 的取值天然表达，不需要额外的计数器。 */

void spinlock_init(osslock_t *lock)
{
    ((spinlock_t *)lock)->lock = 0;
}

/* 自旋锁即申请即用，这里不做额外的死锁预防和处理。 */
irq_key_t spinlock_acquire(osslock_t *lock)
{
    irq_key_t key;
    __local_intr_save(key);
    spinlock_lock((spinlock_t *)lock);
    return key;
}

void spinlock_release(osslock_t *lock, irq_key_t key)
{
    spinlock_unlock((spinlock_t *)lock);
    __local_intr_restore(key);
}

void sem_init(ossem_t *sem, int value)
{
    spinlock_init(&(sem->lock));
    sem->count = value;
    sem->waiting = 0;
    INIT_LIST_HEAD(&(sem->wait_list));
}

void sem_down(ossem_t *sem)
{
    pcb_t *tsk = proc_get_current();
    bool waited = false; /* 是否真的阻塞过；用来让 sem->waiting 的 +1/-1 严格成对 */

    irq_key_t key = spinlock_acquire(&(sem->lock));
    while (sem->count < 1)
    {
        if (!waited)
        {
            atomic_add(&(sem->waiting), 1);
            waited = true;
        }

        /* 只有确实要阻塞时才挂进等待队列；每次循环重新挂一次，
         * 因为上一轮被 sem_up 唤醒时已经把本节点摘掉了 */
        list_add_tail(&(tsk->proc_wait_linker), &(sem->wait_list));

        /* 必须在放掉 sem->lock 之前就把状态置成不可运行（"prepare to wait"，与
         * do_wait() 用的是同一套路，也是 sched.h 里 sleep() 注释要求调用者遵守的约定）：
         * 否则"放锁"到"sleep() 内部赋值状态"之间有一个窗口，另一个 hart 的 sem_up 可以
         * 在这里把本任务 wakeup() 成 RUNNING 并入队，紧接着 sleep() 又把状态覆写回
         * UNINTERRUPTIBLE——这次唤醒就彻底丢了，任务再也不会有人唤醒它（死等）。
         * 先置状态则相反：wakeup 会把它改回 RUNNING，下面 sched_schedule() 看到
         * curr 仍是 RUNNING 就会把它重新入队并继续跑，不会睡死。 */
        tsk->proc_state = UNINTERRUPTIBLE;
        spinlock_release(&(sem->lock), key);
        /* 被 sem_up 唤醒后从这里继续，回到循环开头重新检查条件——可能被虚假唤醒
         * 或被别的任务抢先拿走了信号量，所以不能想当然直接成功 */
        sched_schedule();
        /* 重新取锁：赋值给外层的 key，不能再声明一个同名局部把它遮蔽掉——
         * 那样循环退出后 release 用的会是进入循环前那次 acquire 的陈旧 key。 */
        key = spinlock_acquire(&(sem->lock));
    }

    sem->count -= 1; /* count 最小为 0，不会变负，因为上面 while 保证进这里时 count >= 1 */
    if (waited)
    {
        atomic_add(&(sem->waiting), -1);
    }
    spinlock_release(&(sem->lock), key);
    tsk->proc_state = RUNNING;
}

void sem_up(ossem_t *sem)
{
    irq_key_t key = spinlock_acquire(&(sem->lock));
    sem->count += 1;
    if (!list_empty(&(sem->wait_list)))
    {
        pcb_t *proc;
        struct list_head *wait_entry = (sem->wait_list).next;
        proc = getContainer(wait_entry, pcb_t, proc_wait_linker);
        list_del(&(proc->proc_wait_linker));
        wakeup(proc);
    }
    spinlock_release(&(sem->lock), key);
}

void waitq_init(waitq_t *wq)
{
    INIT_LIST_HEAD(&(wq->task_list));
}

void waitq_prepare(waitq_t *wq)
{
    pcb_t *tsk = proc_get_current();

    list_add_tail(&(tsk->proc_wait_linker), &(wq->task_list));
    /* 必须在调用者放掉条件锁之前完成，理由与 sem_down 里的同名注释一致：
     * "放锁"到"sleep() 内部赋值状态"之间若有窗口，另一个 hart 的
     * waitq_wake_all() 可能在窗口期把本任务唤醒成 RUNNING，随后被这里
     * 覆写回 UNINTERRUPTIBLE，唤醒就此丢失、任务永远睡死。 */
    tsk->proc_state = UNINTERRUPTIBLE;
}

void waitq_prepare_interruptible(waitq_t *wq)
{
    pcb_t *tsk = proc_get_current();

    list_add_tail(&(tsk->proc_wait_linker), &(wq->task_list));
    tsk->proc_state = INTERRUPTIBLE;
}

void waitq_remove(waitq_t *wq, pcb_t *p)
{
    (void)wq;
    /* 用 list_del_init 而不是 list_del：本函数的调用者是"因为收到信号而放弃等待"
     * 的任务，而它同时也可能刚被 waitq_wake_all() 摘走过——那边已经把节点从链上
     * 取下，这里再 del 一次就是操作一对已经指向别处的指针。
     * waitq_prepare/waitq_wake_all 两侧都保证节点要么在链上、要么是自环，
     * 于是重复调用本函数是安全的空操作。 */
    list_del_init(&(p->proc_wait_linker));
}

void waitq_wake_all(waitq_t *wq)
{
    /* 先整体摘链到本地临时头，再逐个唤醒：wakeup() 之后任务可能立刻在另一个
     * hart 上跑起来并重新挂链（比如再次阻塞在同一个 wq 上），若边遍历 wq
     * 边唤醒，遍历用的 next 指针可能已被那次重新挂链改写。摘到本地链表后
     * wq 已清空，重新挂入不会与本次遍历冲突。 */
    struct list_head tmp;
    INIT_LIST_HEAD(&tmp);
    list_splice(&(wq->task_list), &tmp);
    INIT_LIST_HEAD(&(wq->task_list));

    while (!list_empty(&tmp))
    {
        struct list_head *node = tmp.next;
        pcb_t *proc = getContainer(node, pcb_t, proc_wait_linker);
        /* 摘成自环而不是留下悬空指针：被信号打断的任务会用 waitq_remove()
         * 自己再摘一次，那次必须是安全的空操作 */
        list_del_init(node);
        wakeup(proc);
    }
}
