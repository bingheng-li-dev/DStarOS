#include "sync.h"
#include "dassert.h"
#include "sched.h"
#include "proc.h"
#include "containerof.h"

void irq_disable_nesting_increment(void)
{
    bool intr_flag;
    __local_intr_save(intr_flag);
    cpu_t *cpu = getCurrentCpu();
    if (cpu->irqDisableNesting == 0)
    {
        cpu->intrDisableState = intr_flag;
    }
    cpu->irqDisableNesting += 1;
}

void irq_disable_nesting_decrement(void)
{
    cpu_t *cpu = getCurrentCpu();
    dassert(cpu->irqDisableNesting >= 1);
    cpu->irqDisableNesting -= 1;
    /* 当"cpu->intrDisableState"且"cpu->irqDisableNesting"等于0时重新打开中断。 */
    if (cpu->irqDisableNesting == 0)
    {
        __local_intr_restore(cpu->intrDisableState);
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
    pcb_t *tsk = getCurrentProc();
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

        spinlock_release(&(sem->lock));
        /* sleep 内部会调度其他 proc；被 sem_up 唤醒后从这里继续，回到循环开头
         * 重新检查条件——可能被虚假唤醒或被别的任务抢先拿走了信号量，
         * 所以不能想当然直接成功 */
        sleep(tsk, UNINTERRUPTIBLE);
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
