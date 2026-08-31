#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "memtype.h"
#include "tick.h"
#include "sync.h"

osslock_t lock1;
osslock_t lock2;
bool flag = false;

int deadlockTest(int argc, char **args)
{
    printf("Deadlock test!!\n");
    uint64_t coreid = cpu_get_core_id();
    if (coreid == 0)
    {
        spinlock_init(&lock1);
        spinlock_init(&lock2);
        flag = true;
        irq_key_t lock1_key = spinlock_acquire(&lock1);
        tick_delay(1);
        printf("core0 delay finished!\n");
        irq_key_t lock2_key = spinlock_acquire(&lock2);
        printf("core0 Acquire\n");
        spinlock_release(&lock1, lock1_key);
        spinlock_release(&lock2, lock2_key);
        printf("core0 Release\n");
    }
    else
    {
        while (!flag)
            ;
        irq_key_t lock2_key = spinlock_acquire(&lock2);
        tick_delay(1);
        printf("core1 delay finished!\n");
        irq_key_t lock1_key = spinlock_acquire(&lock1);
        printf("core1 Acquire\n");
        spinlock_release(&lock1, lock1_key);
        spinlock_release(&lock2, lock2_key);
        printf("core1 Release\n");
    }
    return 0;
}