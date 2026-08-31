#ifndef _SYNC_H_
#define _SYNC_H_

#include "atomic.h"
#include "trap.h"
#include "list.h"
#include "cpu.h"

typedef struct os_spinlock osslock_t;
typedef struct os_semaphore ossem_t;

struct os_spinlock
{
    spinlock_t spinlock;
};

struct os_semaphore
{
    osslock_t lock;
    int count;                  /* 共享计数值。 */
    int waiting;                /* 等待当前信号量进入睡眠的进程个数。 */
    struct list_head wait_list; /* 当前信号量的等待队列。 */
};

/* 等待队列：供"等条件成立"场景使用（管道、TTY 输入、信号等待等），
 * 与 ossem_t 的计数语义不同——这里没有 count，纯粹是一条 pcb 链表 + 唤醒操作。
 * 本结构自身不带锁：它必须与"被等待的条件"处在同一把锁的保护下，否则"检查条件"
 * 与"挂入队列"之间会有窗口丢失唤醒。调用者持有条件锁时才可调用下列全部函数。 */
typedef struct wait_queue
{
    struct list_head task_list;
} waitq_t;

/* 自旋锁保存的中断状态：acquire 之前中断是否开着（true = 开着，release 时要重新打开）。
 *
 * **必须由调用者自己持有**——局部变量，或者像调度器那样存进 pcb 让它随任务走。
 * 绝不能放回 per-CPU 的槽里，理由见 sync.c 顶部的说明。用法：
 *
 *     irq_key_t key = spinlock_acquire(&lock);
 *     ...
 *     spinlock_release(&lock, key);
 *
 * 嵌套不需要额外的计数器：内层 acquire 存到的就是"进来时已经关着"（false），
 * 它的 release 什么都不做，中断只在最外层那次 release 时才真正打开。 */
typedef bool irq_key_t;

/* 初始化一个自旋锁。 */
void spinlock_init(osslock_t *lock);
/* 获取一个自旋锁并关中断；返回的 key 必须交给配对的 spinlock_release。
 * 获取失败时此函数会让 CPU 进入忙等待。 */
irq_key_t spinlock_acquire(osslock_t *lock);
/* 释放一个自旋锁，并按 key 还原 acquire 之前的中断状态。 */
void spinlock_release(osslock_t *lock, irq_key_t key);
/* 初始化一个信号量。 */
void sem_init(ossem_t *sem, int value);
/* 信号量的P操作；尝试获取一个信号量，获取失败时，此函数会让proc进入睡眠。 */
void sem_down(ossem_t *sem);
/* 信号量的V操作。 */
void sem_up(ossem_t *sem);

/* 初始化一个等待队列。 */
void waitq_init(waitq_t *wq);
/* 把当前任务挂入 wq 并置 UNINTERRUPTIBLE（prepare-to-wait）。调用者须在自己的
 * 条件锁保护下调用；返回后应立即释放条件锁并 sched_schedule()，被唤醒后重新
 * 抢锁、**循环重检条件**（被唤醒不代表条件仍然成立）。 */
void waitq_prepare(waitq_t *wq);
/* 唤醒 wq 上挂着的全部任务并清空队列。调用者须持有条件锁。 */
void waitq_wake_all(waitq_t *wq);

#endif