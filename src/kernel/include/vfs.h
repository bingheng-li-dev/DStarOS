/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _VFS_H_
#define _VFS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "list.h"
#include "types.h"

/* ============================================================
 * 路径与名称长度上限
 * ============================================================ */
#define VFS_PATH_MAX   256   /* 绝对路径最大字节数（含末尾 '\0'）*/
#define VFS_NAME_MAX    64   /* 单个文件名最大字节数（含末尾 '\0'）*/

/* ============================================================
 * 文件打开模式标志（flags，可 OR 组合）
 * ============================================================ */
#define O_RDONLY    0x0000   /* 只读 */
#define O_WRONLY    0x0001   /* 只写 */
#define O_RDWR      0x0002   /* 读写 */
#define O_ACCMODE   0x0003   /* 访问模式掩码 */
#define O_CREAT     0x0040   /* 文件不存在时创建 */
#define O_EXCL      0x0080   /* 配合 O_CREAT：文件已存在则失败 */
#define O_TRUNC     0x0200   /* 打开时截断文件为零长度 */
#define O_APPEND    0x0400   /* 每次写操作追加到文件末尾 */
#define O_NONBLOCK  0x0800   /* 非阻塞模式 */
#define O_DIRECTORY 0x10000  /* 要求路径必须是目录，否则失败（ENOTDIR）*/
#define O_CLOEXEC   0x80000  /* execve 时自动关闭该 fd */

/* ============================================================
 * lseek 参照点
 * ============================================================ */
#define SEEK_SET    0        /* 从文件开头计算偏移 */
#define SEEK_CUR    1        /* 从当前位置计算偏移 */
#define SEEK_END    2        /* 从文件末尾计算偏移 */

/* ============================================================
 * inode 文件类型与权限位
 * ============================================================ */
#define S_IFMT      0xF000   /* 文件类型掩码 */
#define S_IFREG     0x8000   /* 普通文件 */
#define S_IFDIR     0x4000   /* 目录 */
#define S_IFBLK     0x6000   /* 块设备 */
#define S_IFCHR     0x2000   /* 字符设备 */
#define S_IFIFO     0x1000   /* 管道 */
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_IRWXU     0x01C0   /* 拥有者 rwx */
#define S_IRWXG     0x0038   /* 组 rwx */
#define S_IRWXO     0x0007   /* 其他 rwx */

/* ============================================================
 * 前向声明与 typedef
 * ============================================================ */
typedef struct super_block          super_block_t;
typedef struct inode                inode_t;
typedef struct file                 file_t;
typedef struct dentry               dentry_t;
typedef struct vfsmount             vfsmount_t;
typedef struct super_block_operations  super_block_operations_t;
typedef struct inode_operations     inode_operations_t;
typedef struct file_operations      file_operations_t;
typedef struct dentry_operations    dentry_operations_t;
typedef struct file_system_type     file_system_type_t;

/* ============================================================
 * 文件元数据结构体（对应 POSIX stat）
 * ============================================================ */
typedef struct stat stat_t;
struct stat {
    uint64_t  st_ino;    /* inode 编号 */
    uint32_t  st_mode;   /* 文件类型和权限 */
    uint64_t  st_size;   /* 文件大小（字节）*/
    uint32_t  st_nlink;  /* 硬链接数（FAT 文件系统中固定为 1）*/
};

/* ============================================================
 * 超级块（super_block）结构体
 * 代表一个已挂载的文件系统实例，是物理文件系统在内存中的抽象。
 * ============================================================ */
struct super_block
{
    char                    *s_fs_id;           /* 文件系统标识符（如 "fatfs"）*/
    uint64_t                 s_block_size;       /* 文件系统块大小（字节）*/
    uint64_t                 s_total_blocks;     /* 文件系统总块数 */
    file_system_type_t      *s_type;             /* 所属文件系统类型（可为 NULL）*/
    super_block_operations_t *s_op;              /* 超级块操作函数指针 */
    inode_t                 *s_root_inode;       /* 文件系统根目录的 inode */
    void                    *s_private;          /* 具体文件系统的私有数据（如 FATFS 结构体）*/
    vfsmount_t              *s_mount;            /* 对应的挂载点 */
    struct list_head         s_list_linker;      /* 全局超级块链表节点 */
};

/* ============================================================
 * inode（索引节点）结构体
 * 代表文件系统中的一个文件或目录，与具体文件系统无关。
 * ============================================================ */
struct inode
{
    uint64_t            i_ino;      /* inode 编号（FAT 文件系统中为 0，无真实 inode 号）*/
    uint64_t            i_size;     /* 文件大小（字节）*/
    uint32_t            i_mode;     /* 文件类型和权限（S_IFREG/S_IFDIR | 权限位）*/
    inode_operations_t *i_op;       /* inode 操作函数集（目录树操作：create/lookup 等）*/
    /* 文件操作函数集（read/write/open 等），底层文件系统在 alloc_inode 时设置，vfs_open 复制到 file->f_op */
    file_operations_t  *i_fop;
    super_block_t      *i_sb;       /* 所属超级块 */
    dentry_t           *i_dentry;   /* 关联的目录项（FAT 无硬链接，单指针足够）*/
    void               *i_private;  /* 底层文件系统私有数据（fatfs、devfs 目前都不用）*/
};

/* ============================================================
 * 目录项（dentry）结构体
 * 代表文件系统目录树中的一个节点，将文件名映射到 inode。
 *
 * 关键字段语义：
 *   d_subdirs：作为链表"头"，所有子目录项通过各自的 d_child 节点挂入此链表
 *   d_child：  作为链表"节点"，挂入父目录项的 d_subdirs 链表
 *   d_ref：    引用计数 = 外部持有者数量 + 子目录项数量
 *   d_lru：    降为 0 时不立即释放，而是挂到全局 LRU 上等待复用或回收
 *
 * 引用状态与缓存状态是正交的两件事（与 Linux 一致）：
 *   d_ref > 0  —— 正在被使用，一定不在 LRU 上
 *   d_ref == 0 —— 未被使用，挂在 LRU 上，但对象仍然活着、仍挂在父目录的
 *                 d_subdirs 里、仍能被 dentry_lookup 命中并"复活"
 * 由此可推出一条被反复用到的不变式：LRU 上的目录项一定是叶子——它没有子项
 * （否则子项会给它贡献引用），也没有外部持有者。
 * ============================================================ */
struct dentry
{
    char               *d_name;     /* 目录项名称（由 dentry_create 复制，生命周期独立）*/
    inode_t            *d_inode;    /* 对应的 inode（NULL 表示负目录项：已确认不存在）*/
    dentry_t           *d_parent;   /* 父目录项（根目录的 d_parent 指向自身）*/
    struct list_head    d_subdirs;  /* 子目录项链表头——所有子项通过各自的 d_child 挂入 */
    struct list_head    d_child;    /* 此目录项在父目录 d_subdirs 链表中的节点 */
    /* 此目录项在全局 dcache LRU 链表中的节点；孤立（list_empty 为真）表示不在 LRU 上 */
    struct list_head    d_lru;
    dentry_operations_t *d_op;      /* 目录项操作函数指针（可为 NULL）*/
    int                 d_ref;      /* 引用计数；归零时进入 LRU 或被直接回收 */
    vfsmount_t         *d_mounted;  /* 若此目录是挂载点，指向对应 vfsmount，否则为 NULL */
};

/* file 的种类：决定 syscall 壳该不该为它抢 vfs_big_lock。管道/设备类 file 的 read/write
 * 会自己阻塞，套在这把全局睡眠锁里就是持锁睡眠，此后任何文件 syscall 都会卡在同一把锁上。
 * 不能用 f_inode==NULL 判断：/dev/console 是带 inode 的 devfs 节点，read 同样阻塞。 */
typedef enum file_kind
{
    FILE_KIND_VFS = 0,   /* 走 VFS/FatFS 的普通文件与目录：必须持 vfs_big_lock */
    FILE_KIND_PIPE,      /* 管道：自带 pipe->lock，不碰 vfs_big_lock */
    FILE_KIND_DEVICE,    /* 字符设备（TTY、/dev/null、/dev/zero）*/
} file_kind_t;

/* ============================================================
 * 文件对象（file）结构体
 * 代表一个打开的文件实例，与进程相关。
 * ============================================================ */
struct file
{
    char              *f_path;      /* 文件路径字符串（调试用，由 vfs_open 复制）*/
    file_operations_t *f_op;        /* 文件操作函数集（由底层文件系统的 open 回调设置）*/
    inode_t           *f_inode;     /* 指向对应的 inode */
    dentry_t          *f_dentry;    /* 持有目录项引用（防止文件关闭前 dentry 被释放）*/
    vfsmount_t        *f_vfsmount;  /* 文件所在的挂载点 */
    off_t              f_pos;       /* 当前读写位置（64 位，支持大文件）*/
    int                f_mode;      /* 打开模式标志（O_RDONLY/O_WRONLY/O_RDWR 等）*/
    int                f_count;     /* 引用计数；fork / dup 不持锁，一律原子增减 */
    void              *f_private;   /* 底层文件系统私有数据（如 fatfs 的 FIL* 指针）*/
    file_kind_t        f_kind;      /* 是否需要 vfs_big_lock */
};

static inline bool vfs_file_needs_lock(file_t *f)
{
    return f == NULL || f->f_kind == FILE_KIND_VFS;
}

/* ============================================================
 * 挂载点（vfsmount）结构体
 * 代表一个文件系统的挂载实例。
 * ============================================================ */
struct vfsmount
{
    char             *mnt_path;          /* 挂载点路径字符串（如 "/"、"/mnt/fat"）*/
    super_block_t    *mnt_sb;            /* 对应的超级块 */
    dentry_t         *mnt_host_dentry;   /* 宿主文件系统中的挂载点 dentry（".." 跨挂载点回溯用）*/
    struct list_head  mnt_list_linker;   /* 全局挂载点链表节点 */
};

/* ============================================================
 * 文件系统类型结构体
 * 代表一种文件系统（如 fatfs、ext2 等）的类型描述符。
 * ============================================================ */
struct file_system_type
{
    const char         *name;           /* 文件系统名称（如 "fatfs"，不能含 '.'）*/
    /* 挂载回调：创建超级块、根 inode、根 dentry，返回根 dentry；失败返回 NULL */
    dentry_t          *(*mount)(
        file_system_type_t *type,
        const char *source,
        void *data);
    void               (*kill_sb)(super_block_t *sb);  /* 强制卸载超级块 */
    file_system_type_t *next;           /* 已注册文件系统类型单向链表中的下一个 */
};

/* ============================================================
 * 超级块操作接口（对应 Linux super_operations）
 * ============================================================ */
struct super_block_operations
{
    inode_t *(*alloc_inode)(super_block_t *sb);     /* 分配并初始化一个新 inode */
    void     (*destroy_inode)(inode_t *inode);      /* 销毁 inode，释放私有资源 */
    int      (*sync_fs)(super_block_t *sb);         /* 将脏数据同步到底层存储 */
    int      (*unmount)(super_block_t *sb);         /* 卸载文件系统，释放底层资源 */
};

/* ============================================================
 * inode 操作接口（对应 Linux inode_operations）
 * ============================================================ */
struct inode_operations
{
    int       (*create)  (inode_t *dir, dentry_t *dentry, mode_t mode);  /* 创建普通文件 */
    dentry_t *(*lookup)  (inode_t *dir, const char *name);               /* 查找子目录项 */
    int       (*unlink)  (inode_t *dir, dentry_t *dentry);               /* 删除普通文件 */
    int       (*mkdir)   (inode_t *dir, dentry_t *dentry, mode_t mode);  /* 创建目录 */
    int       (*rmdir)   (inode_t *dir, dentry_t *dentry);               /* 删除目录 */
    int       (*rename)  (inode_t *old_dir, dentry_t *old_dentry,        /* 重命名/移动 */
                          inode_t *new_dir, dentry_t *new_dentry);
    int       (*truncate)(inode_t *inode, uint64_t size);                /* 截断/扩展文件 */
};

/* ============================================================
 * 目录项操作接口（对应 Linux dentry_operations）
 * ============================================================ */
struct dentry_operations
{
    int  (*d_revalidate)(dentry_t *dentry);  /* 验证目录项是否仍然有效 */
    void (*d_release)(dentry_t *dentry);     /* 目录项引用归零时的清理回调 */
};

/* ============================================================
 * 文件操作接口（对应 Linux file_operations）
 * ============================================================ */
struct file_operations
{
    ssize_t (*read) (file_t *file, void *buf, size_t len);        /* 读取文件数据 */
    ssize_t (*write)(file_t *file, const void *buf, size_t len);  /* 写入文件数据 */
    int     (*open) (inode_t *inode, file_t *file, int mode);     /* 打开文件（底层初始化）*/
    int     (*close)(file_t *file);                               /* 关闭文件（释放资源）*/
    off_t   (*lseek)(file_t *file, off_t offset, int whence);     /* 移动读写位置 */
    int     (*ioctl)(file_t *file, int cmd, void *arg);           /* 设备控制操作 */
    /* 读目录项：把尽可能多的 struct linux_dirent64 变长记录紧凑填进 buf。
     * 放在 file 层而不是 inode 层，是因为"读到第几项"是打开的目录实例的属性——
     * 同一个目录被两个进程同时打开必须有两个独立游标（Linux 同理，放在
     * file_operations.iterate_shared）。
     * @return 已填字节数；0 表示目录已读完（EOF）；负值为错误码。
     *   缓冲区连一条记录都放不下时返回 ENO6_INVAL_PARAM。 */
    int     (*readdir)(file_t *file, void *buf, size_t len);
};

/* ============================================================
 * 全局根目录项与根挂载点（由 vfs_mount("/", ...) 设置）
 * ============================================================ */
extern dentry_t   *vfs_root_dentry;
extern vfsmount_t *vfs_root_mount;

/* ============================================================
 * 引用计数导出接口（供 proc.c 与自检用例使用）
 * ============================================================ */
void dentry_get_pub(dentry_t *d);   /* 引用计数 +1 */
void dentry_put_pub(dentry_t *d);   /* 引用计数 -1，归零时进入 LRU 或回收 */

/* ============================================================
 * 目录项缓存（dcache）：可观测性与内存压力接口
 * ============================================================ */

/* LRU 上允许驻留的未使用目录项数量。单条目约 dentry + d_name + inode ≈ 180 字节，
 * 128 条约 22 KB。超过 MAX 时在 dentry_put 里批量回收到 LOW，避免"超一个收一个"的抖动。 */
#define DCACHE_MAX_UNUSED   128
#define DCACHE_LOW_WATER     96

typedef struct dcache_stats
{
    uint64_t hits;       /* dentry_lookup 命中（含在 LRU 上被复活的）*/
    uint64_t misses;     /* dentry_lookup 未命中，落到 i_op->lookup */
    uint64_t revives;    /* 从 LRU 上复活的次数 */
    uint64_t evicts;     /* 真正释放（kfree）的次数 */
    uint32_t nr_unused;  /* 当前 LRU 上的条目数 */
} dcache_stats_t;

void vfs_dcache_stats(void);                        /* 打印统计 */
void vfs_dcache_get_stats(dcache_stats_t *out);     /* 取统计快照（自检用）*/
void vfs_dcache_shrink(uint32_t nr);                /* 回收至多 nr 条（须持 vfs 大锁）*/
void vfs_dcache_reclaim(void);                      /* kmalloc 重试路径的回收回调 */

/* ============================================================
 * VFS 对外接口函数声明
 * ============================================================ */

/* 初始化 */
void vfs_init(void);

/* VFS 大锁（睡眠信号量）：保护 dentry/inode 缓存树与 FatFS 卷内部状态（win[] 扇区缓存、
 * FAT 表等，FatFS 本身 _FS_REENTRANT=0 不可重入）。只在"进入 VFS 的入口"加锁——
 * vfs.c 内部函数之间互相调用不重复加锁，否则非重入信号量会自锁死。 */
void vfs_lock(void);
void vfs_unlock(void);

/* 文件系统注册与注销 */
int16_t register_filesystem(file_system_type_t *fs_type);
int16_t unregister_filesystem(file_system_type_t *fs_type);

/* 挂载与卸载 */
int vfs_mount(const char *path, const char *fs_type, void *data);
int vfs_unmount(const char *path);

/* 文件操作 */
file_t *vfs_open        (const char *path, int mode, int *err);
int     vfs_close       (file_t *file);
ssize_t vfs_read        (file_t *file, void *buf, size_t len);
ssize_t vfs_write       (file_t *file, const void *buf, size_t len);
int     vfs_truncate    (const char *path, uint64_t size);  /* 截断/扩展文件到指定大小 */
int     vfs_ftruncate   (file_t *file, uint64_t size);      /* 同上，但按已打开的 file 定位（不经路径）*/
off_t   vfs_lseek       (file_t *file, off_t offset, int whence);

/* 目录操作 */
int vfs_mkdir  (const char *path, mode_t mode);
int vfs_rmdir  (const char *path);
int vfs_rename (const char *oldpath, const char *newpath);
int vfs_link   (const char *oldpath, const char *newpath);     /* 创建硬链接（FAT 不支持）*/
int vfs_symlink(const char *target, const char *linkpath);     /* 创建符号链接（FAT 不支持）*/
int vfs_unlink (const char *path);
int vfs_chdir  (const char *path);
int vfs_getcwd (char *buf, size_t size);

/* 文件信息 */
int vfs_stat    (const char *path, stat_t *statbuf);
int vfs_fstat   (file_t *file, stat_t *statbuf);  /* 已打开文件版本，直接读 f_inode，无需再解析路径 */

/* 读目录项（getdents64 的 VFS 层入口）：转发给 f_op->readdir。
 * 返回已填字节数，0 = 目录读完，负值为错误码。 */
int vfs_getdents(file_t *file, void *buf, size_t len);

/* 路径解析（返回持有一个引用计数的 dentry，调用者负责 dentry_put_pub）*/
dentry_t *vfs_lookup(const char *path);

/* 超级块操作（VFS 内部接口，由具体文件系统适配层调用）*/
super_block_t *alloc_super_block(
    const char *fs_id,
    uint64_t block_size,
    uint64_t total_blocks,
    super_block_operations_t *s_op,
    void *private_data);
void destroy_super_block(super_block_t *sb);

/* inode 操作（VFS 内部接口）*/
inode_t *alloc_inode(super_block_t *sb);
void     destroy_inode(inode_t *inode);

/* 目录项操作（VFS 内部接口）*/
dentry_t *dentry_create(const char *name, inode_t *inode,
                        dentry_t *parent, dentry_operations_t *op);
dentry_t *dentry_lookup(dentry_t *parent, const char *name);

#endif /* _VFS_H_ */
