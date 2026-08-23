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

/* 等待队列：供"等条件成立"场景使用（管道、Phase 5 TTY 输入、Phase 7 信号等待），
 * 与 ossem_t 的计数语义不同——这里没有 count，纯粹是一条 pcb 链表 + 唤醒操作。
 * 本结构自身不带锁：它必须与"被等待的条件"处在同一把锁的保护下，否则"检查条件"
 * 与"挂入队列"之间会有窗口丢失唤醒。调用者持有条件锁时才可调用下列全部函数。 */
typedef struct wait_queue
{
    struct list_head task_list;
} waitq_t;

/* 中断请求屏蔽嵌套增加1;必须与"irq_disable_nesting_decrement"成对使用。 */
void irq_disable_nesting_increment(void);
/* 中断请求屏蔽嵌套减少1;必须与"irq_disable_nesting_increment"成对使用。 */
void irq_disable_nesting_decrement(void);
/* 初始化一个自旋锁。 */
void spinlock_init(osslock_t *lock);
/* 获取一个自旋锁，获取失败时，此函数会让CPU进入忙等待。 */
void spinlock_acquire(osslock_t *lock);
/* 释放一个自旋锁。 */
void spinlock_release(osslock_t *lock);
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