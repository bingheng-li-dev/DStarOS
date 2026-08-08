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

#endif