#include "pmm.h"
#include "kmalloc.h"

extern pframe_t *microPhysicalMemoryPoolBase;

void *kmalloc(uint64_t size)
{
    void *ret;
    if (microPhysicalMemoryPoolBase != NULL && size <= 1024)
    {
        ret = microAlloc(size);
        if (ret != NULL)
        {
            return ret;
        }
    }
    /* "microPhysicalMemoryPoolBase" is null or "size" is more than 1024 or no remaining micro mem in pool. */
    ret = pmm_alloc(convert_pa2ppn_cil(size)); /* Here "convert" is used to calculate amount of pframes. */
    return (void *)convert_pframe2pa(ret);
}

void kfree(void *ptr)
{
    if (ptr == NULL)
    {
        goto f1;
    }
    if (microPhysicalMemoryPoolBase != NULL)
    {
        if (convert_pframe2pa(microPhysicalMemoryPoolBase) < (phyAddr_t)ptr && (phyAddr_t)ptr < convert_pframe2pa(microPhysicalMemoryPoolBase) + 2 * PGSIZE)
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
    pmm_dealloc(base);

f1:
    return;
}
