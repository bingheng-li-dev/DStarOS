/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "sync.h"
#include "pmm.h"
#include "debug.h"

#if DEBUG_VMM_self_test
extern void vmm_test(void);
#endif

/* vfs_test() 曾经挂在这里，条件是 cpu_get_core_id() == 0——但 main() 只有 hart 1
 * 会执行（hart 0 的 proc_init() 成为 init 进程后永不返回，init.c 里那句 main()
 * 走不到），两个条件互斥，那套 VFS/FatFS 回归实际上从来没被跑过。现在跟其它
 * 内核态自检一样由 proc_init() 调用，见 proc.c 的 DEBUG_VFS_TEST 分支。 */

int main(int argc, char **args)
{
#if DEBUG_VMM_self_test
    vmm_test();
#endif

    return 0;
}