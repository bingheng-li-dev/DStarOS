#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "sync.h"
#include "pmm.h"
#include "debug.h"

#if DEBUG_VMM_self_test
extern void vmm_test(void);
#endif

int main(int argc, char **args)
{
#if DEBUG_VMM_self_test
    vmm_test();
#endif

    return 0;
}