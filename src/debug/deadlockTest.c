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
    uint64_t coreid = getCoreId();
    if (coreid == 0)
    {
        spinlockInit(&lock1);
        spinlockInit(&lock2);
        flag = true;
        spinlockAcquire(&lock1);
        delay(1);
        printf("core0 delay finished!\n");
        spinlockAcquire(&lock2);
        printf("core0 Acquire\n");
        spinlockRelease(&lock1);
        spinlockRelease(&lock2);
        printf("core0 Release\n");
    }
    else
    {
        while (!flag)
            ;
        spinlockAcquire(&lock2);
        delay(1);
        printf("core1 delay finished!\n");
        spinlockAcquire(&lock1);
        printf("core1 Acquire\n");
        spinlockRelease(&lock1);
        spinlockRelease(&lock2);
        printf("core1 Release\n");
    }
    return 0;
}