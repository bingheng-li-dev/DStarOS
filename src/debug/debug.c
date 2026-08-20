#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "sync.h"
#include "pmm.h"
#include "debug.h"
#if DEBUG_VFS_TEST
#include "cpu.h"
#endif

#if DEBUG_VMM_self_test
extern void vmm_test(void);
#endif
#if DEBUG_VFS_TEST
extern void vfs_test(void);
#endif

int main(int argc, char **args)
{
#if DEBUG_VMM_self_test
    vmm_test();
#endif

#if DEBUG_VFS_TEST
    /* 只在 hart0 跑：main() 被两个 hart 各调用一次，两边同时跑会并发建/删同一批
     * 测试文件，互相把对方的前置条件破坏掉 */
    if (cpu_get_core_id() == 0)
    {
        vfs_test();
    }
#endif

    return 0;
}