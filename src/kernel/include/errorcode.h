#ifndef _ERRORCODE_H_
#define _ERRORCODE_H_

#define ENO0_NO_ERROR 0
#define ENO1_NOMORE_MEM -1
#define ENO2_ALLOCPROC_FAILED -2
#define ENO3_NOFREE_PID -3
#define ENO4_BUSY -4         /* Device/File is Busy. */
#define ENO5_NOSUCH_ENTRY -5 /* No Such File or Directory. */
#define ENO6_INVAL_PARAM -6  /* Invalid parameter. */
#define ENO7_EXISTS -7       /* File/Directory Already Exists. */

#define ENO8_NULL_POINTER -8 /* Null Pointer Exception. */

/* 文件系统 VFS 专用错误码 */
#define ENO9_NOT_DIR        -9    /* 路径中某分量不是目录（ENOTDIR）*/
#define ENO10_IS_DIR       -10    /* 目标是目录，但该操作不适用于目录（EISDIR）*/
#define ENO11_NAME_TOO_LONG -11   /* 文件名或路径名过长（ENAMETOOLONG）*/
#define ENO12_NOT_EMPTY    -12    /* 目录非空，无法删除（ENOTEMPTY）*/
#define ENO13_NO_FS        -13    /* 没有已挂载的根文件系统（ENODEV）*/
#define ENO14_CROSS_DEV    -14    /* 跨挂载点重命名/移动（EXDEV）*/
#define ENO15_READ_ONLY    -15    /* 文件系统只读（EROFS）*/
#define ENO16_PERM         -16    /* 操作不被允许（EPERM）*/
#define ENO17_NO_CHILD     -17    /* 没有子进程可等待（ECHILD）*/

#define ENO18_TOO_MANY_FILES -18  /* fd表满了 */

#endif
