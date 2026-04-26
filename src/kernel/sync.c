#include "sync.h"
#include "dassert.h"
#include "sched.h"
#include "proc.h"
#include "containerof.h"

void irqDisableNestingIncrement(void)
{
    bool intr_flag;
    __localIntrSave(intr_flag);
    cpu_t *cpu = getCurrentCpu();
    if (cpu->irqDisableNesting == 0)
    {
        cpu->intrDisableState = intr_flag;
    }
    cpu->irqDisableNesting += 1;
}

void irqDisableNestingDecrement(void)
{
    cpu_t *cpu = getCurrentCpu();
    dassert(cpu->irqDisableNesting >= 1);
    cpu->irqDisableNesting -= 1;
    /* 当"cpu->intrDisableState"且"cpu->irqDisableNesting"等于0时重新打开中断。 */
    if (cpu->irqDisableNesting == 0)
    {
        __localIntrRestore(cpu->intrDisableState);
    }
}

void spinlockInit(osslock_t *lock)
{
    ((spinlock_t *)lock)->lock = 0;
}

/* 自旋锁即申请即用，这里不做额外的死锁预防和处理。 */
// trylock?
/* corelock_lock：获取核间锁，核之间互斥的锁，同核内该锁会嵌套，
只有异核之间会阻塞。不建议在中断使用该函数，中断中可以使用corelock_trylock。*/
/* 申请spinlock并且保存irq状况。 */
void spinlockAcquire(osslock_t *lock)
{
    irqDisableNestingIncrement();
    spinlock_lock((spinlock_t *)lock);
}

/* 释放spinlock并且还原irq状况。 */
void spinlockRelease(osslock_t *lock)
{
    spinlock_unlock((spinlock_t *)lock);
    irqDisableNestingDecrement();
}

void semInit(ossem_t *sem, int value)
{
    spinlockInit(&(sem->lock));
    sem->count = value;
    sem->waiting = 0;
    INIT_LIST_HEAD(&(sem->wait_list));
}

void semDown(ossem_t *sem)
{
    pcb_t *tsk = getCurrentProc();
    /* 先全部加到等待队列中再说；此处只是单纯模仿了linux源码的做法。 */
    tsk->proc_state = UNINTERRUPTIBLE;
    /* 使用"list_add_tail"，因为"wait_list"是一个队列。 */
    list_add_tail(&(tsk->proc_list_linker), &(sem->wait_list));
    atomic_add(&(sem->waiting), 1);
    spinlockAcquire(&(sem->lock));
    while (1)
    {
        if (sem->count >= 1) /* 如果成功获取到信号量的情况。 */
        {
            sem->count -= 1; /* 注意count的最小值为0，不会出现负数的情况，与下面semUp函数仅在等待队列为空的情形相对应。 */
            atomic_add(&(sem->waiting), -1);
            break;
        }
        /* 如果没有成功获取到信号量的情况。 */
        spinlockRelease(&(sem->lock));
        /* 在前面已经使得proc睡眠，这里调度其他的proc。 */
        sched();
        /* proc在被唤醒之后继续执行while循环进行信号量获取测试。 */
        spinlockAcquire(&(sem->lock));
        tsk->proc_state = UNINTERRUPTIBLE;
    }
    spinlockRelease(&(sem->lock));
    tsk->proc_state = RUNNING;
}

void semUp(ossem_t *sem)
{
    spinlockAcquire(&(sem->lock));
    sem->count += 1;
    if (!list_empty(&(sem->wait_list)))
    {
        pcb_t *proc;
        struct list_head *wait_entry = (sem->wait_list).next;
        proc = getContainer(wait_entry, pcb_t, proc_list_linker);
        list_del(&(proc->proc_list_linker));
        wakeup(proc);
    }
    spinlockRelease(&(sem->lock));
}
