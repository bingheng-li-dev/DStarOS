/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "ktime.h"
#include "proc.h"
#include "signal.h"
#include "sync.h"
#include "list.h"
#include "console.h"

/* 内核启动那一刻的 time CSR 快照。ktime_get_ns() 减掉它，单调钟从 0 起步，
 * 即 uptime（POSIX 只要求单调，这是为了可读）。 */
static uint64_t boot_time_counts;

/* 墙钟相对单调钟的偏移，CLOCK_REALTIME = ktime_get_ns() + 本值。
 * 必须是有符号的：用户可以把时间设回 1970，写成无符号会绕成天文数字。
 * rv64 上对齐的 64 位读写本身原子，读侧不加锁；写侧（clock_settime）极稀有，
 * 两个 hart 同时设时间的结果是"其中一个赢"，这个语义可以接受。 */
static int64_t realtime_offset_ns;

/* 按 proc_alarm_expire_ns 升序排列的 ITIMER_REAL 定时器链表。静态初始化：
 * ktime_check_alarms() 由 tick 中断无条件调用，可能早于任何运行时初始化。
 * 不与 sleeping_tasks 合用：进程可以同时睡在 nanosleep 里又装着定时器，
 * 共用一个 list_head 成员等于让同一个节点挂两条链。 */
static struct list_head alarm_list = LIST_HEAD_INIT(alarm_list);
static osslock_t alarm_list_lock;

/**
 * @brief 记下启动时刻的 time CSR 基准，墙钟从编译期纪元起步
 */
void ktime_init(void)
{
    boot_time_counts = tick_read_time();
    realtime_offset_ns = (int64_t)(DSTAROS_BUILD_EPOCH_SEC * NSEC_PER_SEC);
    printf("ktime: boot epoch %ld sec, timebase %ld Hz\n",
           (long)DSTAROS_BUILD_EPOCH_SEC, (long)TIMEBASE_FREQ_HZ);
}

/**
 * @brief 初始化 ITIMER_REAL 定时器链表的锁
 */
void ktime_alarm_init(void)
{
    spinlock_init(&alarm_list_lock);
}

/**
 * @brief 自内核启动以来的纳秒数，即 CLOCK_MONOTONIC
 */
uint64_t ktime_get_ns(void)
{
    return counts_to_ns(tick_read_time() - boot_time_counts);
}

/**
 * @brief CLOCK_REALTIME：ktime_get_ns() 加上一个可写的偏移量
 */
uint64_t ktime_get_real_ns(void)
{
    return (uint64_t)((int64_t)ktime_get_ns() + realtime_offset_ns);
}

/**
 * @brief 设置墙钟：反算并改写偏移量。CLOCK_MONOTONIC 不受影响
 */
void ktime_set_real_ns(uint64_t real_ns)
{
    realtime_offset_ns = (int64_t)real_ns - (int64_t)ktime_get_ns();
}

/* 按到期时刻升序插入 alarm_list；调用者必须持 alarm_list_lock */
static void alarm_insert_locked(pcb_t *p)
{
    struct list_head *pos;
    list_for_each(pos, &alarm_list)
    {
        pcb_t *q = list_entry(pos, pcb_t, proc_alarm_linker);
        if (q->proc_alarm_expire_ns > p->proc_alarm_expire_ns)
        {
            break;
        }
    }
    list_add_tail(&p->proc_alarm_linker, pos);
}

/* 算剩余时间；调用者必须持 alarm_list_lock。已到期但还没被投递的返回 1 ns 而不是 0——
 * getitimer 的 0 表示"没装定时器"，装着的定时器不能报 0。 */
static uint64_t alarm_remaining_locked(pcb_t *p, uint64_t now)
{
    if (p->proc_alarm_expire_ns == 0)
    {
        return 0;
    }
    if (p->proc_alarm_expire_ns <= now)
    {
        return 1;
    }
    return p->proc_alarm_expire_ns - now;
}

/**
 * @brief 装/改一个进程的 ITIMER_REAL 定时器（参数见 ktime.h）
 */
void ktime_alarm_set(pcb_t *p, uint64_t expire_ns, uint64_t interval_ns,
                     uint64_t *old_value, uint64_t *old_interval)
{
    uint64_t now = ktime_get_ns();
    irq_key_t key = spinlock_acquire(&alarm_list_lock);

    if (old_value != NULL)
    {
        *old_value = alarm_remaining_locked(p, now);
    }
    if (old_interval != NULL)
    {
        *old_interval = p->proc_alarm_interval_ns;
    }

    if (!list_empty(&p->proc_alarm_linker))
    {
        list_del_init(&p->proc_alarm_linker);
    }

    p->proc_alarm_expire_ns = expire_ns;
    p->proc_alarm_interval_ns = (expire_ns == 0) ? 0 : interval_ns;
    if (expire_ns != 0)
    {
        alarm_insert_locked(p);
    }

    spinlock_release(&alarm_list_lock, key);
}

/**
 * @brief 读一个进程的定时器剩余时间与周期（纳秒），未装时两者均为 0
 */
void ktime_alarm_get(pcb_t *p, uint64_t *value, uint64_t *interval)
{
    uint64_t now = ktime_get_ns();
    irq_key_t key = spinlock_acquire(&alarm_list_lock);
    if (value != NULL)
    {
        *value = alarm_remaining_locked(p, now);
    }
    if (interval != NULL)
    {
        *interval = p->proc_alarm_interval_ns;
    }
    spinlock_release(&alarm_list_lock, key);
}

/**
 * @brief 取消定时器并摘链；幂等，未装时是空操作
 */
void ktime_alarm_cancel(pcb_t *p)
{
    irq_key_t key = spinlock_acquire(&alarm_list_lock);
    if (!list_empty(&p->proc_alarm_linker))
    {
        list_del_init(&p->proc_alarm_linker);
    }
    p->proc_alarm_expire_ns = 0;
    p->proc_alarm_interval_ns = 0;
    spinlock_release(&alarm_list_lock, key);
}

/**
 * @brief 向所有到期的进程投 SIGALRM，并重排周期定时器
 * @details 由 tick_int_handler() 每次 tick 调用，运行在中断上下文。signal_send() 只做
 *   "置位 + 条件唤醒"，不睡眠、不分配内存，可以在这里调（tty 的 ^C → SIGINT 走的是同一条路径）。
 *
 *   锁序 alarm_list_lock → sighand->lock → run_queue.lock，单向；本函数在
 *   sched_task_tick() 释放 run_queue.lock 之后才被调用，不会反转。
 *
 *   周期定时器要把到期时刻推到下一个未来时刻再插回：interval 小于 tick 周期时必须
 *   用 while 补齐，否则那个节点会在同一次遍历里反复到期，在中断上下文里死循环。
 *   重新插入的节点 expire > now，遍历走到它时会被 break 挡住。
 */
void ktime_check_alarms(void)
{
    uint64_t now = ktime_get_ns();
    struct list_head *pos, *tmp;

    irq_key_t key = spinlock_acquire(&alarm_list_lock);
    list_for_each_safe(pos, tmp, &alarm_list)
    {
        pcb_t *p = list_entry(pos, pcb_t, proc_alarm_linker);
        if (p->proc_alarm_expire_ns > now)
        {
            break;
        }
        list_del_init(pos);

        if (p->proc_alarm_interval_ns != 0)
        {
            uint64_t expire = p->proc_alarm_expire_ns;
            while (expire <= now)
            {
                expire += p->proc_alarm_interval_ns;
            }
            p->proc_alarm_expire_ns = expire;
            alarm_insert_locked(p);
        }
        else
        {
            p->proc_alarm_expire_ns = 0;
        }

        signal_send(p, SIGALRM);
    }
    spinlock_release(&alarm_list_lock, key);
}
