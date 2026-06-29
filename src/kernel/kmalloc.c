#include "pmm.h"
#include "kmalloc.h"
#include "memtype.h"
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
    /* MMU 开启后调用者使用 KVA，MMU 关闭时用 PA */
    phyAddr_t frame_pa = convert_pframe2pa(tmp);
    ret = mmu_is_enabled() ? (void *)pa_to_kva(frame_pa) : (void *)frame_pa;
done:
    spinlockRelease(&PmmLock);
    return ret;
}

void kfree(void *ptr)
{
    spinlockAcquire(&PmmLock);
    if (ptr == NULL)
        goto f1;

    /* MMU 开启后外部指针均为 KVA，归一化到 PA 以统一做范围判断和 pframe 换算 */
    phyAddr_t ptr_pa = mmu_is_enabled()
                       ? kva_to_pa((virAddr_t)ptr)
                       : (phyAddr_t)ptr;

    if (MicroPhysicalMemoryPoolBase != NULL)
    {
        phyAddr_t pool_pa = convert_pframe2pa(MicroPhysicalMemoryPoolBase);
        if (pool_pa < ptr_pa && ptr_pa < pool_pa + 2 * PGSIZE)
        {
            /* microDemalloc 内部通过原始指针（KVA 或 PA）操作 pool */
            if (microDemalloc(ptr))
                goto f1;
        }
    }
    /* Maybe memories in pool have ran out. */
    pframe_t *base = convert_pa2pframe_flr(ptr_pa);
    dealloc(base);
f1:
    spinlockRelease(&PmmLock);
}
