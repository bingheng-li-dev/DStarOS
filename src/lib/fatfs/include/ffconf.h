/*---------------------------------------------------------------------------/
/  FatFs - FAT file system module configuration file  R0.11 (C)ChaN, 2015
/---------------------------------------------------------------------------*/

#define _FFCONF 32020	/* Revision ID */

/*---------------------------------------------------------------------------/
/ Functions and Buffer Configurations
/---------------------------------------------------------------------------*/

#define	_FS_TINY		0
/* This option switches tiny buffer configuration. (0:Normal or 1:Tiny)
/  At the tiny configuration, size of the file object (FIL) is reduced _MAX_SS
/  bytes. Instead of private sector buffer eliminated from the file object,
/  common sector buffer in the file system object (FATFS) is used for the file
/  data transfer. */


#define _FS_READONLY	0
/* This option switches read-only configuration. (0:Read/Write or 1:Read-only)
/  Read-only configuration removes writing API functions, f_write(), f_sync(),
/  f_unlink(), f_mkdir(), f_chmod(), f_rename(), f_truncate(), f_getfree()
/  and optional writing functions as well. */


#define _FS_MINIMIZE	0
/* This option defines minimization level to remove some basic API functions.
/
/   0: All basic functions are enabled.
/   1: f_stat(), f_getfree(), f_unlink(), f_mkdir(), f_chmod(), f_utime(),
/      f_truncate() and f_rename() function are removed.
/   2: f_opendir(), f_readdir() and f_closedir() are removed in addition to 1.
/   3: f_lseek() function is removed in addition to 2. */


#define	_USE_STRFUNC	1
/* This option switches string functions, f_gets(), f_putc(), f_puts() and
/  f_printf().
/
/  0: Disable string functions.
/  1: Enable without LF-CRLF conversion.
/  2: Enable with LF-CRLF conversion. */


#define _USE_FIND		0
/* This option switches filtered directory read feature and related functions,
/  f_findfirst() and f_findnext(). (0:Disable or 1:Enable) */


#define	_USE_MKFS		1
/* 已修改为 1：启用 f_mkfs() 格式化功能（用于 QEMU ramdisk 首次格式化）*/


#define	_USE_FASTSEEK	0
/* This option switches fast seek feature. (0:Disable or 1:Enable) */


#define _USE_LABEL		0
/* This option switches volume label functions, f_getlabel() and f_setlabel().
/  (0:Disable or 1:Enable) */


#define	_USE_FORWARD	0
/* This option switches f_forward() function. (0:Disable or 1:Enable)
/  To enable it, also _FS_TINY need to be set to 1. */


/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define _CODE_PAGE	437
/* 已修改为 437（US ASCII）：原 936（GBK）需要 option/cc936.c 字码表文件，
/  该文件不在项目中。437 不需要额外字码表文件。
/
/   1    - ASCII (No extended character. Non-LFN cfg. only)
/   437  - U.S.
/   ...
/   936  - Simplified Chinese GBK (DBCS)
*/


#define	_USE_LFN	3
#define	_MAX_LFN	255
/* 已改为 3（堆分配缓冲区）：1 是静态缓冲不可重入，双 hart/多进程并发访问会互相踩踏；
/  2 是栈上缓冲，_MAX_LFN=255 时约需 512+ 字节栈，而本内核栈只有 1 页 4KB
/  （KERNEL_STACKPSIZE 1），FatFS 调用链本来就深，风险太高。3 需要实现
/  ff_memalloc/ff_memfree（fatfs_vfs.c 转发到 kmalloc/kfree）。*/
/* The _USE_LFN option switches the LFN feature.
/
/   0: Disable LFN feature. _MAX_LFN has no effect.
/   1: Enable LFN with static working buffer on the BSS. Always NOT thread-safe.
/   2: Enable LFN with dynamic working buffer on the STACK.
/   3: Enable LFN with dynamic working buffer on the HEAP. */


#define	_LFN_UNICODE	0
/* This option switches character encoding on the API. (0:ANSI/OEM or 1:Unicode) */


#define _STRF_ENCODE	3
/* When _LFN_UNICODE is 1, this option selects the character encoding on the file. */


#define _FS_RPATH	0
/* This option configures relative path feature.
/  VFS 层负责相对路径解析，fatfs 不需要此功能，保持 0。
/
/   0: Disable relative path feature and remove related functions.
/   1: Enable relative path feature. f_chdir() and f_chdrive() are available.
/   2: f_getcwd() function is available in addition to 1. */


/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define _VOLUMES	2
/* Number of volumes (logical drives) to be used. */


#define _STR_VOLUME_ID	0
#define _VOLUME_STRS	"RAM","NAND","CF","SD1","SD2","USB1","USB2","USB3"


#define	_MULTI_PARTITION	0


#define	_MIN_SS		512
#define	_MAX_SS		512


#define	_USE_TRIM	0


#define _FS_NOFSINFO	0


/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define _FS_NORTC	1
#define _NORTC_MON	1
#define _NORTC_MDAY	1
#define _NORTC_YEAR	2024
/* 已修改为 1：系统无 RTC，禁用时间戳功能，使用固定时间戳。
/  原值为 0 时需要实现 get_fattime()，嵌入式内核中通常无 RTC。*/


#define	_FS_LOCK	0


#define _FS_REENTRANT	0
#define _FS_TIMEOUT		1000
#define	_SYNC_t			int
/* 禁用 fatfs 内部重入锁（由 VFS 层的自旋锁负责并发控制）*/


#define _WORD_ACCESS	0
/* RISC-V 不保证非对齐访问，保持 0（字节对齐访问）*/
