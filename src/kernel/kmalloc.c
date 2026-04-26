#include "pmm.h"
#include "kmalloc.h"
#include "sync.h"

extern pframe_t *MicroPhysicalMemoryPoolBase;
extern osslock_t PmmLock;

void *kmalloc(uint64_t size)
{
    void *ret = NULL, *tmp = NULL;
    spinlockAcquire(&PmmLock);
    if (MicroPhysicalMemoryPoolBase != NULL && size <= 1024)
    {
        tmp = microAlloc(size);
        if (tmp != NULL)
        {
            ret = tmp;
            goto done;
        }
        /* microAlloc pool exhausted, fall through to PMM page allocator */
    }
    /* "MicroPhysicalMemoryPoolBase" is null, size > 1024, or micro pool is full */
    tmp = alloc(convert_pa2ppn_cil(size)); /* Here "convert" is used to calculate amount of pframes. */
    ret = (void *)convert_pframe2pa(tmp);
done:
    spinlockRelease(&PmmLock);
    return ret;
}

void kfree(void *ptr)
{
    spinlockAcquire(&PmmLock);
    if (ptr == NULL)
    {
        goto f1;
    }
    if (MicroPhysicalMemoryPoolBase != NULL)
    {
        if (convert_pframe2pa(MicroPhysicalMemoryPoolBase) < (phyAddr_t)ptr && (phyAddr_t)ptr < convert_pframe2pa(MicroPhysicalMemoryPoolBase) + 2 * PGSIZE)
        {
            if (microDemalloc(ptr))
            {
                goto f1;
            }
        }
    }
    /* Maybe memories in pool have ran out. */
    pframe_t *base;
    base = convert_pa2pframe_flr((phyAddr_t)ptr); /* Here "convert" is used to calculate amount of the pframe. */
    dealloc(base);
f1:
    spinlockRelease(&PmmLock);
    return;
}
