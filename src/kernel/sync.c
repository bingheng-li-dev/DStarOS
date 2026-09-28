/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "sync.h"
#include "dassert.h"
#include "sched.h"
#include "proc.h"
#include "containerof.h"

/* 自旋锁保存的中断状态由调用者持有（内核栈上的局部变量，或调度器存进 pcb 的
 * proc_rq_key），不能放 per-CPU 的槽：acquire 与 release 不保证在同一条执行流、
 * 同一个 hart 上完成，run_queue.lock 就是跨 switch_to 接力释放的。
 * 嵌套无需计数器：内层 acquire 拿到的 key 是"已关"，它的 release 不会开中断。 */

/**
 * @brief 初始化一个自旋锁
 */
void spinlock_init(osslock_t *lock)
{
    ((spinlock_t *)lock)->lock = 0;
}

/**
 * @brief 获取一个自旋锁并关中断；返回的 key 必须交给配对的 spinlock_release
 * @note 即申请即用，不做额外的死锁预防和处理。
 */
irq_key_t spinlock_acquire(osslock_t *lock)
{
    irq_key_t key;
    __local_intr_save(key);
    spinlock_lock((spinlock_t *)lock);
    return key;
}

/**
 * @brief 释放一个自旋锁，并按 key 还原 acquire 之前的中断状态
 */
void spinlock_release(osslock_t *lock, irq_key_t key)
{
    spinlock_unlock((spinlock_t *)lock);
    __local_intr_restore(key);
}

/**
 * @brief 初始化一个信号量
 */
void sem_init(ossem_t *sem, int value)
{
    spinlock_init(&(sem->lock));
    sem->count = value;
    sem->waiting = 0;
    INIT_LIST_HEAD(&(sem->wait_list));
}

/**
 * @brief 信号量的P操作；尝试获取一个信号量，获取失败时让当前任务进入睡眠
 */
void sem_down(ossem_t *sem)
{
    pcb_t *tsk = proc_get_current();
    /* 是否真的阻塞过；用来让 sem->waiting 的 +1/-1 严格成对 */
    bool waited = false;

    irq_key_t key = spinlock_acquire(&(sem->lock));
    while (sem->count < 1)
    {
        if (!waited)
        {
            atomic_add(&(sem->waiting), 1);
            waited = true;
        }

        /* 每轮重新挂：上一轮被 sem_up 唤醒时本节点已被摘掉 */
        list_add_tail(&(tsk->proc_wait_linker), &(sem->wait_list));

        /* 放锁之前就置不可运行（prepare to wait）：否则放锁到 sleep 之间，另一个 hart 的
         * sem_up 可能先把本任务 wakeup 成 RUNNING，随后又被覆写回 UNINTERRUPTIBLE，
         * 唤醒丢失、任务睡死。先置状态则 sched_schedule() 会看到 RUNNING 并继续跑。 */
        tsk->proc_state = UNINTERRUPTIBLE;
        spinlock_release(&(sem->lock), key);
        /* 被唤醒不代表拿得到（可能被别的任务抢先），回循环重检 */
        sched_schedule();
        /* 重新取锁：赋值给外层的 key，不能再声明一个同名局部把它遮蔽掉——
         * 那样循环退出后 release 用的会是进入循环前那次 acquire 的陈旧 key。 */
        key = spinlock_acquire(&(sem->lock));
    }

    sem->count -= 1;
    if (waited)
    {
        atomic_add(&(sem->waiting), -1);
    }
    spinlock_release(&(sem->lock), key);
    tsk->proc_state = RUNNING;
}

/**
 * @brief 信号量的V操作
 */
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

/**
 * @brief 初始化一个等待队列
 */
void waitq_init(waitq_t *wq)
{
    INIT_LIST_HEAD(&(wq->task_list));
}

/**
 * @brief 把当前任务挂入 wq 并置 UNINTERRUPTIBLE（prepare-to-wait）；调用者须持条件锁
 */
void waitq_prepare(waitq_t *wq)
{
    pcb_t *tsk = proc_get_current();

    list_add_tail(&(tsk->proc_wait_linker), &(wq->task_list));
    /* 必须在调用者放掉条件锁之前置状态：否则窗口期内 waitq_wake_all() 置的 RUNNING
     * 会被随后的赋值覆写回去，唤醒丢失、任务睡死。 */
    tsk->proc_state = UNINTERRUPTIBLE;
}

/**
 * @brief 同 waitq_prepare，但置 INTERRUPTIBLE：等待期间可以被信号唤醒
 */
void waitq_prepare_interruptible(waitq_t *wq)
{
    pcb_t *tsk = proc_get_current();

    list_add_tail(&(tsk->proc_wait_linker), &(wq->task_list));
    tsk->proc_state = INTERRUPTIBLE;
}

/**
 * @brief 把 p 从 wq 上摘下来（若它还在）；调用者须持条件锁，重复调用是安全的空操作
 */
void waitq_remove(waitq_t *wq, pcb_t *p)
{
    (void)wq;
    /* 调用者因信号放弃等待时，节点可能刚被 waitq_wake_all() 摘过。两侧都保证节点
     * 要么在链上、要么是自环，所以用 list_del_init，重复调用是安全的空操作。 */
    list_del_init(&(p->proc_wait_linker));
}

/**
 * @brief 唤醒 wq 上挂着的全部任务并清空队列；调用者须持条件锁
 */
void waitq_wake_all(waitq_t *wq)
{
    /* 先整体摘到本地链表再逐个唤醒：被唤醒的任务可能立刻在另一个 hart 上重新挂回
     * 同一个 wq，边遍历边唤醒会让遍历用的 next 指针被改写。 */
    struct list_head tmp;
    INIT_LIST_HEAD(&tmp);
    list_splice(&(wq->task_list), &tmp);
    INIT_LIST_HEAD(&(wq->task_list));

    while (!list_empty(&tmp))
    {
        struct list_head *node = tmp.next;
        pcb_t *proc = getContainer(node, pcb_t, proc_wait_linker);
        /* 摘成自环：被信号打断的任务还会 waitq_remove() 再摘一次 */
        list_del_init(node);
        wakeup(proc);
    }
}
