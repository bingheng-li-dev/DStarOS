#include "sync.h"
#include "dassert.h"
#include "sched.h"
#include "proc.h"
#include "containerof.h"

void irq_disable_nesting_increment(void)
{
    bool intr_flag;
    __local_intr_save(intr_flag);
    cpu_t *cpu = cpu_get_current();
    if (cpu->irq_disable_nesting == 0)
    {
        cpu->intr_disable_state = intr_flag;
    }
    cpu->irq_disable_nesting += 1;
}

void irq_disable_nesting_decrement(void)
{
    cpu_t *cpu = cpu_get_current();
    dassert(cpu->irq_disable_nesting >= 1);
    cpu->irq_disable_nesting -= 1;
    /* 当"cpu->intr_disable_state"且"cpu->irq_disable_nesting"等于0时重新打开中断。 */
    if (cpu->irq_disable_nesting == 0)
    {
        __local_intr_restore(cpu->intr_disable_state);
    }
}

void spinlock_init(osslock_t *lock)
{
    ((spinlock_t *)lock)->lock = 0;
}

/* 自旋锁即申请即用，这里不做额外的死锁预防和处理
 * 申请spinlock并且保存irq状况。 */
void spinlock_acquire(osslock_t *lock)
{
    irq_disable_nesting_increment();
    spinlock_lock((spinlock_t *)lock);
}

/* 释放spinlock并且还原irq状况。 */
void spinlock_release(osslock_t *lock)
{
    spinlock_unlock((spinlock_t *)lock);
    irq_disable_nesting_decrement();
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

    spinlock_acquire(&(sem->lock));
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
        spinlock_release(&(sem->lock));
        /* 被 sem_up 唤醒后从这里继续，回到循环开头重新检查条件——可能被虚假唤醒
         * 或被别的任务抢先拿走了信号量，所以不能想当然直接成功 */
        sched_schedule();
        spinlock_acquire(&(sem->lock));
    }

    sem->count -= 1; /* count 最小为 0，不会变负，因为上面 while 保证进这里时 count >= 1 */
    if (waited)
    {
        atomic_add(&(sem->waiting), -1);
    }
    spinlock_release(&(sem->lock));
    tsk->proc_state = RUNNING;
}

void sem_up(ossem_t *sem)
{
    spinlock_acquire(&(sem->lock));
    sem->count += 1;
    if (!list_empty(&(sem->wait_list)))
    {
        pcb_t *proc;
        struct list_head *wait_entry = (sem->wait_list).next;
        proc = getContainer(wait_entry, pcb_t, proc_wait_linker);
        list_del(&(proc->proc_wait_linker));
        wakeup(proc);
    }
    spinlock_release(&(sem->lock));
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
        list_del(node);
        wakeup(proc);
    }
}
