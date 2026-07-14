#ifndef _SYSCALL_H_
#define _SYSCALL_H_

#include "trap.h"

/* Linux风格的系统调用号，NR为Number */
#define __NR_write 64
#define __NR_exit  93

/* Linux riscv64 的 syscall 接口约定返回类型就是 long */
long syscall_dispatch(intstkf_t *sp);   /* a7=号, a0..a5=参, 返回值即写回 a0 */

#endif
