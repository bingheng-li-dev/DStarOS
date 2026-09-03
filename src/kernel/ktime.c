#include "ktime.h"
#include "proc.h"
#include "signal.h"
#include "sync.h"
#include "list.h"
#include "console.h"

/* 内核启动那一刻的 time CSR 快照。ktime_get_ns() 减掉它，于是单调钟从 0 起步——
 * POSIX 只要求单调，直接返回 CSR 原值也合法，但减掉基准之后数字可读得多，
 * 而且顺带就是 uptime。 */
static uint64_t boot_time_counts;

/* 墙钟相对单调钟的偏移，CLOCK_REALTIME = ktime_get_ns() + 本值。
 * **必须是有符号的**：用户可以把时间设回 1970，写成无符号会绕成天文数字。
 * rv64 上对齐的 64 位读写本身原子，读侧不加锁；写侧（clock_settime）极稀有，
 * 两个 hart 同时设时间的结果是"其中一个赢"，这个语义可以接受。 */
static int64_t realtime_offset_ns;

/* 按 proc_alarm_expire_ns 升序排列的 ITIMER_REAL 定时器链表。
 * 静态初始化的理由与 sched.c 的 sleeping_tasks 完全相同：tick_int_handler() 里
 * ktime_check_alarms() 是无条件调用的，BSS 清零的链表头会让 list_for_each_safe
 * 当场解引用 NULL。
 * **不与 sleeping_tasks 合用**：一个进程可以同时睡在 nanosleep 里、又装着定时器，
 * 共用一个 list_head 成员等于让同一个节点挂两条链（proc.h 明令禁止）。 */
static struct list_head alarm_list = LIST_HEAD_INIT(alarm_list);
static osslock_t alarm_list_lock;

void ktime_init(void)
{
    boot_time_counts = tick_read_time();
    realtime_offset_ns = (int64_t)(DSTAROS_BUILD_EPOCH_SEC * NSEC_PER_SEC);
    printf("ktime: boot epoch %ld sec, timebase %ld Hz\n",
           (long)DSTAROS_BUILD_EPOCH_SEC, (long)TIMEBASE_FREQ_HZ);
}

void ktime_alarm_init(void)
{
    spinlock_init(&alarm_list_lock);
}

uint64_t ktime_get_ns(void)
{
    return counts_to_ns(tick_read_time() - boot_time_counts);
}

uint64_t ktime_get_real_ns(void)
{
    return (uint64_t)((int64_t)ktime_get_ns() + realtime_offset_ns);
}

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
 * @details 由 tick_int_handler() 每次 tick 调用，运行在中断上下文。
 *   从中断里调 signal_send() 是安全的——阶段 7 的 ^C → SIGINT 走的就是
 *   tick_int_handler → tty_poll_input → signal_send_group 这条同样的路径，
 *   signal_send 内部只做"置位 + 条件唤醒"，不睡眠、不分配内存。
 *
 *   锁序：alarm_list_lock → sighand->lock → run_queue.lock，单向。本函数由
 *   tick_int_handler 在 sched_task_tick() 释放 run_queue.lock 之后调用，
 *   不会形成反转。
 *
 *   周期定时器到期后就地重排：先把到期时刻推到**下一个未来时刻**再插回链表。
 *   interval 小于 tick 周期时必须用 while 补齐而不是只加一次，否则那个节点会在
 *   同一次遍历里反复到期——**死循环在中断上下文里**。重新插入的节点由于
 *   expire > now，遍历走到它时会被下面的 break 挡住，不会被二次处理。
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
