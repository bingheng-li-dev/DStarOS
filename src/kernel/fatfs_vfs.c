/**
 * fatfs_vfs.c - FatFS ↔ VFS 适配层
 *
 * 将 ChaN FatFS R0.11 的路径式 API 桥接到 DStarOS VFS 的四层抽象接口。
 *
 * 设计映射关系：
 *   VFS 抽象          FatFS 对应           实现方式
 *   ─────────────────────────────────────────────────────
 *   super_block_t  ←→  FATFS 对象          FATFS* 存入 sb->s_private
 *   inode_t        ←→  路径字符串          由 fatfs_build_path() 沿 dentry 链现推（不缓存）
 *   dentry_t       ←→  路径分量            由 VFS dentry_create 管理
 *   file_t         ←→  FIL 对象            FIL* 存入 file->f_private
 *
 * QEMU 平台使用 ramdisk（见 diskio.c），首次挂载时若无有效 FAT 则自动格式化。
 */

#include "fatfs_vfs.h"
#include "slab.h"
#include "vfs.h"
#include "ff.h"
#include "diskio.h"
#include "kmalloc.h"
#include "errorcode.h"
#include "stringops.h"
#include "console.h"
#include "linux_abi.h"

/* ============================================================
 * 私有数据结构
 * ============================================================ */

/* inode 不再持有私有数据（i_private 恒为 NULL）。
 *
 * 曾经这里放的是 fatfs_inode_priv_t，用一个 char path[VFS_PATH_MAX] 缓存该文件在
 * 卷内的绝对路径。那是一份**冗余状态**——同样的信息 dentry 树里已经有了，而且
 * 一旦路径变化就必须逐个同步：`f_rename` 之后只有被重命名的那个 inode 的路径被
 * 更新，它整棵子树里的后代全部指向旧路径，`rename("/a/b","/a/d")` 之后
 * `open("/a/d/c.txt")` 会拿 "/a/b/c.txt" 去问 FatFS。
 *
 * 现在改为需要时用 fatfs_build_path() 沿 d_parent 现推。少一份要同步的状态，
 * 顺带每个 inode 省下 256 字节——这是目录项缓存单条目成本的大头。 */

/* 合成阶段机的取值：FatFS 的 f_readdir 会跳过 FAT 目录里真实存在的 "." / ".." 项
 * （ff.c 的 dir_read 在 _FS_RPATH=0 时过滤掉所有以 '.' 开头的条目），而
 * BusyBox 的 ls -a / rm -r / find 都期待它们出现，只能由适配层自己补上。 */
#define FATFS_DIR_SYNTH_DOT    0  /* 待吐 "." */
#define FATFS_DIR_SYNTH_DOTDOT 1  /* 待吐 ".." */
#define FATFS_DIR_SYNTH_DONE   2  /* 两条都吐完了，进入 f_readdir 循环 */

/**
 * fatfs_dir_priv_t - 打开的**目录** file 的私有数据（挂在 file->f_private）
 *
 * 注意与普通文件的区别：普通文件的 f_private 是 `FIL*`，目录是这个结构体。
 * 两者靠 `S_ISDIR(file->f_inode->i_mode)` 区分——vfs_open 在调用 f_op->open 之前
 * 就已经填好了 file->f_inode，所以各回调里都能安全地据此判断，不会类型混淆。
 */
typedef struct
{
    DIR      dir;                        /* FatFS 目录对象 */
    int      synth;                      /* 合成阶段，见 FATFS_DIR_SYNTH_* */
    uint64_t next_off;                   /* 已返回条目数，用作 d_off 游标与 d_ino 的来源 */
    /* 从 f_readdir 读出来、却发现调用方缓冲区放不下的那一条：它已经被 FatFS 的
     * 游标消费掉了，不暂存下来就会永久丢失，所以必须缓存到下次调用先吐出去。
     * 这里存"解析后的名字"而不是整个 FILINFO——FILINFO.lfname 是指向 lfn_buf 的
     * 指针，整体拷贝只会拷到指针本身，而 lfn_buf 下一次 f_readdir 就被覆写了。 */
    bool     has_pending;
    bool     pending_is_dir;
    char     pending_name[_MAX_LFN + 1];
    /* 每次 f_readdir 取长文件名用的工作缓冲（FILINFO.lfname 指向这里）*/
    char     lfn_buf[_MAX_LFN + 1];
} fatfs_dir_priv_t;

/* 全局 FATFS 对象（f_mount 要求持久存在直至卸载）*/
static FATFS fatfs_obj;  // @todo 全局变量命名风格统一

static super_block_operations_t fatfs_sb_ops;
static inode_operations_t       fatfs_inode_ops;
static file_operations_t        fatfs_file_ops;

/* diskio.c 中导出的扇区总数查询函数 */
extern unsigned int ramdisk_get_sector_count(void);

/* ============================================================
 * FatFS 内存分配钩子（_USE_LFN=3 需要，见 ffconf.h）
 * ============================================================ */

void *ff_memalloc(UINT msize)
{
    return kmalloc(msize);
}

void ff_memfree(void *mblock)
{
    kfree(mblock);
}

/**
 * @brief Unicode 转大写（_USE_LFN 需要，比较/生成短文件名时用）
 * @note 只处理 ASCII a-z——项目不含完整 Unicode 大小写表，且本内核的目标文件名
 *   （BusyBox 等）全部是 ASCII，其余字符原样返回足够正确。
 */
WCHAR ff_wtoupper(WCHAR chr)
{
    if (chr >= 'a' && chr <= 'z')
    {
        return (WCHAR)(chr - 0x20);
    }
    return chr;
}

/**
 * @brief OEM 码页 ↔ Unicode 双向转换（_USE_LFN 需要）
 * @param[in] chr 待转换字符
 * @param[in] dir 0 = Unicode→OEM，1 = OEM→Unicode
 * @return 转换结果；0 表示无法转换（FatFS 约定，调用方会拒绝该名字或退化显示为 '?'）
 * @note 项目未附带 `_CODE_PAGE 437` 的官方转换表（ChaN 发行版的 option/cc437.c 不在本仓库），
 *   且本内核的目标文件名全部是 ASCII——单字节码页里 ASCII 范围（<0x80）在 OEM 与 Unicode
 *   下逐字节相同，直接原样返回即可正确处理所有实际会用到的文件名；0x80 以上的扩展字符
 *   （如 437 的制表符、重音字母）没有表可查，一律按"无法转换"处理。**已知限制**：
 *   非 ASCII 文件名会被拒绝创建/显示为 '?'，本项目场景下可接受。
 */
WCHAR ff_convert(WCHAR chr, UINT dir)
{
    (void)dir;
    if (chr < 0x80)
    {
        return chr;
    }
    return 0;
}

/* ============================================================
 * 路径转换辅助函数
 * ============================================================ */

/**
 * @brief 将 VFS 绝对路径转换为 FatFS 卷路径
 * @param[in]  vfs_path VFS 绝对路径（"" 表示根，"/dir/f" 表示普通路径）
 * @param[out] buf      输出缓冲区
 * @param[in]  bufsz    缓冲区大小
 * @note "" → "0:/"；"/dir/f" → "0:/dir/f"
 */
static void vfs_to_fatfs_path(const char *vfs_path, char *buf, int bufsz)
{
    if (!vfs_path || vfs_path[0] == '\0')
    {
        /* 根目录 */
        if (bufsz >= 4)
        {
            buf[0] = '0'; buf[1] = ':'; buf[2] = '/'; buf[3] = '\0';
        }
        return;
    }

    /* "0:" + vfs_path（已以 '/' 开头）*/
    int plen = (int)strlen(vfs_path);
    if (2 + plen + 1 > bufsz)
    {
        buf[0] = '\0';
        return;
    }
    buf[0] = '0';
    buf[1] = ':';
    memcpy(buf + 2, vfs_path, plen + 1);
}

/**
 * @brief 构造子路径：parent_path + "/" + name
 * @param[in]  parent_path 父路径（"" 表示根）
 * @param[in]  name        子条目名称
 * @param[out] buf         输出缓冲区
 * @param[in]  bufsz       缓冲区大小
 * @note 若 parent_path 为 ""（根），则子路径为 "/name"。
 */
static void fatfs_make_child_path(const char *parent_path,
                                   const char *name,
                                   char *buf, int bufsz)
{
    int plen = (int)strlen(parent_path);
    int nlen = (int)strlen(name);

    if (plen == 0)
    {
        /* 父路径为根：子路径 = "/name" */
        if (1 + nlen + 1 > bufsz)
        {
            buf[0] = '\0';
            return;
        }
        buf[0] = '/';
        memcpy(buf + 1, name, nlen + 1);
    }
    else
    {
        /* 一般情况：parent_path + "/" + name */
        if (plen + 1 + nlen + 1 > bufsz)
        {
            buf[0] = '\0';
            return;
        }
        memcpy(buf, parent_path, plen);
        buf[plen] = '/';
        memcpy(buf + plen + 1, name, nlen + 1);
    }
}

/**
 * @brief 沿 d_parent 上溯，拼出目录项在本卷内的绝对路径
 * @param[in]  d     目标目录项
 * @param[out] buf   输出缓冲区
 * @param[in]  bufsz 缓冲区大小
 * @retval ENO0_NO_ERROR     成功；卷根本身得到空字符串 ""（与 vfs_to_fatfs_path 的约定一致）
 * @retval ENO5_NOSUCH_ENTRY 这条链上有目录项已被 unlink/rmdir 脱链，路径无意义
 * @retval ENO11_NAME_TOO_LONG 拼出来超过 bufsz
 * @details 倒着往缓冲区尾部写再整体前移，免掉一个用来反转分量顺序的辅助栈——
 *   内核栈只有一页，递归或变长数组都不合适。
 * @note 终点必须是本文件系统的根 inode。脱链的目录项同样以"d_parent 指向自身"
 *   标记，光看循环退出条件区分不了，不检查的话会把一条张冠李戴的路径交给 FatFS。
 */
static int fatfs_build_path(const dentry_t *d, char *buf, int bufsz)
{
    if (d == NULL || bufsz < 1)
    {
        return ENO8_NULL_POINTER;
    }

    int pos = bufsz;
    buf[--pos] = '\0';

    while (d->d_parent != d)
    {
        int nlen = (int)strlen(d->d_name);
        if (pos < nlen + 1)
        {
            return ENO11_NAME_TOO_LONG;
        }
        pos -= nlen;
        memcpy(buf + pos, d->d_name, nlen);
        buf[--pos] = '/';
        d = d->d_parent;
    }

    if (d->d_inode == NULL || d->d_inode->i_sb == NULL ||
        d->d_inode->i_sb->s_root_inode != d->d_inode)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    memmove(buf, buf + pos, bufsz - pos);
    return ENO0_NO_ERROR;
}

/**
 * @brief 拼出 inode 对应的 FatFS 卷路径（"0:/dir/f"）
 * @param[in]  inode 目标 inode
 * @param[out] buf   输出缓冲区，容量需 >= VFS_PATH_MAX + 4
 * @param[in]  bufsz 缓冲区大小
 * @return ENO0_NO_ERROR 或 fatfs_build_path 的错误码
 */
static int fatfs_inode_fatfs_path(const inode_t *inode, char *buf, int bufsz)
{
    char vfs_path[VFS_PATH_MAX];

    if (inode == NULL)
    {
        return ENO8_NULL_POINTER;
    }
    int ret = fatfs_build_path(inode->i_dentry, vfs_path, sizeof(vfs_path));
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    vfs_to_fatfs_path(vfs_path, buf, bufsz);
    return ENO0_NO_ERROR;
}

/* ============================================================
 * FRESULT → VFS 错误码转换
 * ============================================================ */

static int fresult_to_vfs(FRESULT fr)
{
    switch (fr)
    {
    case FR_OK:              return ENO0_NO_ERROR;
    case FR_NO_FILE:
    case FR_NO_PATH:         return ENO5_NOSUCH_ENTRY;
    case FR_EXIST:           return ENO7_EXISTS;
    case FR_WRITE_PROTECTED: return ENO15_READ_ONLY;
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:   return ENO13_NO_FS;
    case FR_NOT_ENOUGH_CORE: return ENO1_NOMORE_MEM;
    case FR_DENIED:          return ENO16_PERM;
    case FR_INVALID_NAME:    return ENO11_NAME_TOO_LONG;
    default:                 return ENO6_INVAL_PARAM;
    }
}

/* ============================================================
 * inode 分配与释放（内部辅助函数）
 * ============================================================ */

/**
 * @brief 分配并基本初始化一个 fatfs inode
 * @param[in] sb 所属超级块
 * @return 新 inode 指针；内存不足返回 NULL
 * @note i_mode 和 i_size 未设置，由调用者负责填写。
 */
static inode_t *fatfs_alloc_inode_internal(super_block_t *sb)
{
    inode_t *inode = (inode_t *)slab_cache_alloc(inode_cache);
    if (!inode)
    {
        return NULL;
    }

    inode->i_private = NULL;   /* 路径由 fatfs_build_path 现推，无需私有数据 */
    inode->i_sb      = sb;
    inode->i_op      = &fatfs_inode_ops;
    inode->i_fop     = &fatfs_file_ops;
    inode->i_dentry  = NULL;
    inode->i_ino     = 0;    /* FAT 无真实 inode 号 */
    inode->i_size    = 0;
    inode->i_mode    = 0;

    return inode;
}

/* ============================================================
 * 超级块操作回调实现
 * ============================================================ */

static inode_t *fatfs_alloc_inode_cb(super_block_t *sb)
{
    return fatfs_alloc_inode_internal(sb);
}

static void fatfs_destroy_inode_cb(inode_t *inode)
{
    if (!inode)
    {
        return;
    }
    kfree(inode);   /* fatfs 的 inode 不带私有数据，i_private 恒为 NULL */
}

static int fatfs_sync_fs_cb(super_block_t *sb)
{
    (void)sb;
    /* FatFS 写操作已即时同步到 ramdisk/SD 卡，无需额外操作 */
    return ENO0_NO_ERROR;
}

static int fatfs_unmount_cb(super_block_t *sb)
{
    (void)sb;
    f_mount(NULL, "0:", 0);  /* 卸载 FatFS 卷，释放内部状态 */
    return ENO0_NO_ERROR;
}

static super_block_operations_t fatfs_sb_ops = {
    .alloc_inode   = fatfs_alloc_inode_cb,
    .destory_inode = fatfs_destroy_inode_cb,
    .sync_fs       = fatfs_sync_fs_cb,
    .unmount       = fatfs_unmount_cb,
};

/* ============================================================
 * 挂载回调（file_system_type_t.mount）
 * ============================================================ */

/**
 * @brief FatFS 挂载回调入口
 * @param[in] fst    文件系统类型描述符（未使用）
 * @param[in] source 设备路径（未使用，驱动号固定为 0）
 * @param[in] data   额外挂载参数（未使用）
 * @return 根目录的 dentry 指针；失败返回 NULL
 * @details 步骤：disk_initialize → f_mount（无 FAT 时先 f_mkfs）
 *          → alloc_super_block → 分配根 inode → dentry_create。
 */
static dentry_t *fatfs_mount_cb(file_system_type_t *fst,
                                 const char *source,
                                 void *data)
{
    (void)fst;
    (void)source;
    (void)data;

    /* 1. 初始化磁盘驱动 */
    DSTATUS dstat = disk_initialize(0);
    if (dstat & STA_NOINIT)
    {
        printf("fatfs_mount: disk init failed, dstat=0x%x\n", (unsigned)dstat);
        return NULL;
    }

    /* 2. 挂载 FatFS 卷（opt=1：立即强制挂载，读取 BPB）*/
    FRESULT fr = f_mount(&fatfs_obj, "0:", 1);
    if (fr == FR_NO_FILESYSTEM)
    {
        /* 无有效 FAT 文件系统，格式化 ramdisk */
        printf("fatfs_mount: no filesystem, formatting ramdisk...\n");
        BYTE work[512];
        fr = f_mkfs("0:", 0, sizeof(work));
        if (fr != FR_OK)
        {
            printf("fatfs_mount: f_mkfs failed, fr=%d\n", (int)fr);
            return NULL;
        }
        /* 重新挂载格式化后的卷 */
        fr = f_mount(&fatfs_obj, "0:", 1);
    }
    if (fr != FR_OK)
    {
        printf("fatfs_mount: f_mount failed, fr=%d\n", (int)fr);
        return NULL;
    }

    /* 3. 创建超级块 */
    unsigned int sector_count = ramdisk_get_sector_count();
    super_block_t *sb = alloc_super_block(
        "fatfs",
        512,
        (uint64_t)sector_count,
        &fatfs_sb_ops,
        &fatfs_obj);
    if (!sb)
    {
        f_mount(NULL, "0:", 0);
        return NULL;
    }

    /* 4. 创建根 inode（路径为空字符串，代表 FAT 卷根）*/
    inode_t *root_inode = fatfs_alloc_inode_internal(sb);
    if (!root_inode)
    {
        destroy_super_block(sb);
        f_mount(NULL, "0:", 0);
        return NULL;
    }
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_size = 0;
    /* 卷根的路径是空字符串，由 fatfs_build_path 在循环一次不跑时自然得到 */

    /* 5. 创建根 dentry（parent=NULL 表示文件系统局部根，d_parent 指向自身）*/
    dentry_t *root_dentry = dentry_create("", root_inode, NULL, NULL);
    if (!root_dentry)
    {
        fatfs_destroy_inode_cb(root_inode);
        destroy_super_block(sb);
        f_mount(NULL, "0:", 0);
        return NULL;
    }
    root_inode->i_dentry = root_dentry;
    sb->s_root_inode     = root_inode;

    printf("fatfs_mount: mounted ok (%u sectors)\n", sector_count);
    return root_dentry;
}

/* ============================================================
 * inode 操作回调实现
 * ============================================================ */

/**
 * @brief 在目录 dir 下查找名称为 name 的子条目
 * @param[in] dir  父目录 inode
 * @param[in] name 要查找的文件/目录名称
 * @return 持有引用计数的子目录项；未找到返回 NULL
 * @note 调用 f_stat 检查存在性，存在则创建 inode + dentry 加入缓存。
 */
static dentry_t *fatfs_lookup_cb(inode_t *dir, const char *name)
{
    /* 构造子条目的 VFS 路径和 FatFS 路径 */
    char dir_vfs[VFS_PATH_MAX];
    char child_vfs[VFS_PATH_MAX];
    char child_fatfs[VFS_PATH_MAX + 4];
    if (fatfs_build_path(dir->i_dentry, dir_vfs, sizeof(dir_vfs)) != ENO0_NO_ERROR)
    {
        return NULL;
    }
    fatfs_make_child_path(dir_vfs, name, child_vfs, VFS_PATH_MAX);
    vfs_to_fatfs_path(child_vfs, child_fatfs, sizeof(child_fatfs));

    /* 查询文件/目录是否存在。_USE_LFN 开启后 FILINFO 多出 lfname/lfsize 两个字段，
     * f_stat 内部按 "if (fno->lfname)" 判断是否要取长文件名——这里用不到长名
     * （name 已经是调用方给定的分量），显式清零使 lfname=NULL 关闭该分支，
     * 避免栈上未初始化的 lfname 被当成有效指针写入越界。 */
    FILINFO finfo;
    memset(&finfo, 0, sizeof(finfo));
    FRESULT fr = f_stat(child_fatfs, &finfo);
    if (fr != FR_OK)
    {
        return NULL;
    }

    /* 创建并填写 inode */
    inode_t *inode = fatfs_alloc_inode_internal(dir->i_sb);
    if (!inode)
    {
        return NULL;
    }

    inode->i_size = (uint64_t)finfo.fsize;
    /* FAT 没有权限概念，统一给可执行位——ash 执行程序前拿 st_mode & 0111 判断能不能
     * 执行，普通文件不给可执行位的话 /bin/busybox 这类可执行文件会被拒绝运行。
     * AM_RDO（FAT 只读属性）映射成去掉写位的 0555，其余情况统一 0755。 */
    mode_t perm = (finfo.fattrib & AM_RDO) ? 0555 : 0755;
    inode->i_mode = (finfo.fattrib & AM_DIR)
                    ? (S_IFDIR | perm) : (S_IFREG | perm);

    /* 创建 dentry，挂入父目录（dir->i_dentry 是父 dentry）*/
    dentry_t *d = dentry_create(name, inode, dir->i_dentry, NULL);
    if (!d)
    {
        fatfs_destroy_inode_cb(inode);
        return NULL;
    }
    inode->i_dentry = d;

    return d;
}

/**
 * @brief 创建普通文件
 * @param[in] dir    父目录 inode
 * @param[in] dentry 预分配的负目录项（d_inode 为 NULL）
 * @param[in] mode   文件权限位（未使用，固定为 S_IFREG|0755，见函数体注释）
 * @retval ENO0_NO_ERROR 成功，dentry->d_inode 已填写
 * @note 调用 f_open(FA_CREATE_NEW) 在磁盘上建立文件。
 */
static int fatfs_create_cb(inode_t *dir, dentry_t *dentry, mode_t mode)
{
    (void)mode;

    /* dentry 此刻已由 dentry_create 挂进 dir 的子链表，直接从它上溯即可 */
    char child_vfs[VFS_PATH_MAX];
    char child_fatfs[VFS_PATH_MAX + 4];
    int pret = fatfs_build_path(dentry, child_vfs, sizeof(child_vfs));
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(child_vfs, child_fatfs, sizeof(child_fatfs));

    /* 在磁盘上创建文件 */
    FIL fil;
    FRESULT fr = f_open(&fil, child_fatfs, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }
    f_close(&fil);

    /* 为 dentry 创建并填写 inode */
    inode_t *inode = fatfs_alloc_inode_internal(dir->i_sb);
    if (!inode)
    {
        return ENO1_NOMORE_MEM;
    }

    /* 统一给可执行位——见 fatfs_lookup_cb 里同样改动的注释（ash 靠 st_mode & 0111
     * 判断能否执行，普通文件不给可执行位会导致 /bin/busybox 这类程序被拒绝运行）。
     * 刚创建的文件不可能带 FAT 只读属性，不用像 lookup_cb 那样查 AM_RDO。 */
    inode->i_mode   = S_IFREG | 0755;
    inode->i_size   = 0;
    inode->i_dentry = dentry;
    dentry->d_inode = inode;  /* 填写之前为 NULL 的负目录项 */

    return ENO0_NO_ERROR;
}

/**
 * @brief 创建目录
 * @param[in] dir    父目录 inode
 * @param[in] dentry 预分配的负目录项
 * @param[in] mode   目录权限位（未使用，固定为 S_IFDIR|0755）
 * @retval ENO0_NO_ERROR 成功，dentry->d_inode 已填写
 * @note 调用 f_mkdir 在磁盘上建立目录。
 */
static int fatfs_mkdir_cb(inode_t *dir, dentry_t *dentry, mode_t mode)
{
    (void)mode;

    char child_vfs[VFS_PATH_MAX];
    char child_fatfs[VFS_PATH_MAX + 4];
    int pret = fatfs_build_path(dentry, child_vfs, sizeof(child_vfs));
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(child_vfs, child_fatfs, sizeof(child_fatfs));

    FRESULT fr = f_mkdir(child_fatfs);
    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }

    inode_t *inode = fatfs_alloc_inode_internal(dir->i_sb);
    if (!inode)
    {
        return ENO1_NOMORE_MEM;
    }

    inode->i_mode   = S_IFDIR | 0755;
    inode->i_size   = 0;
    inode->i_dentry = dentry;
    dentry->d_inode = inode;

    return ENO0_NO_ERROR;
}

/**
 * @brief 删除普通文件
 * @param[in] dir    父目录 inode（未使用）
 * @param[in] dentry 要删除的目录项
 * @return VFS 错误码（ENO0_NO_ERROR 成功）
 * @note FatFS 的 f_unlink 对文件和空目录均适用。
 */
static int fatfs_unlink_cb(inode_t *dir, dentry_t *dentry)
{
    (void)dir;

    char vfs_path[VFS_PATH_MAX];
    char fatfs_path[VFS_PATH_MAX + 4];
    int pret = fatfs_build_path(dentry, vfs_path, sizeof(vfs_path));
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(vfs_path, fatfs_path, sizeof(fatfs_path));

    return fresult_to_vfs(f_unlink(fatfs_path));
}

/**
 * @brief 删除空目录（委托给 fatfs_unlink_cb 实现）
 * @param[in] dir    父目录 inode
 * @param[in] dentry 要删除的目录项
 * @return VFS 错误码（ENO0_NO_ERROR 成功）
 */
static int fatfs_rmdir_cb(inode_t *dir, dentry_t *dentry)
{
    return fatfs_unlink_cb(dir, dentry);
}

/**
 * @brief 重命名/移动文件或目录
 * @param[in] old_dir    源父目录 inode（未使用）
 * @param[in] old_dentry 源目录项
 * @param[in] new_dir    目标父目录 inode
 * @param[in] new_dentry 目标目录项（提供新名称）
 * @retval ENO0_NO_ERROR 成功，old_dentry 对应 inode 路径已更新
 */
static int fatfs_rename_cb(inode_t *old_dir, dentry_t *old_dentry,
                            inode_t *new_dir, dentry_t *new_dentry)
{
    (void)old_dir;
    (void)new_dir;

    /* 两条路径都从各自的 dentry 上溯得到：old_dentry 还挂在原父目录下，
     * new_dentry 是 vfs_rename 预先挂到新父目录下的负目录项。 */
    char old_vfs[VFS_PATH_MAX];
    char new_vfs[VFS_PATH_MAX];
    char old_fatfs[VFS_PATH_MAX + 4];
    char new_fatfs[VFS_PATH_MAX + 4];

    int pret = fatfs_build_path(old_dentry, old_vfs, sizeof(old_vfs));
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    pret = fatfs_build_path(new_dentry, new_vfs, sizeof(new_vfs));
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(old_vfs, old_fatfs, sizeof(old_fatfs));
    vfs_to_fatfs_path(new_vfs, new_fatfs, sizeof(new_fatfs));

    FRESULT fr = f_rename(old_fatfs, new_fatfs);
    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }

    /* 不需要再回头改任何 inode 里存的路径：路径不再被缓存，vfs_rename 把 dentry
     * 挂到新父目录之后，整棵子树的路径自然全部跟着变。 */
    return ENO0_NO_ERROR;
}

/**
 * @brief 截断或扩展文件到指定大小
 * @param[in] inode 目标文件 inode
 * @param[in] size  目标大小（字节）；0 清空；大于当前大小时扩展
 * @retval ENO0_NO_ERROR 成功，inode->i_size 已更新
 * @note 使用 FA_WRITE 打开 → f_lseek(size) → f_truncate()。
 *       FAT32 文件大小上限为 4 GB（DWORD 限制）。
 */
static int fatfs_truncate_cb(inode_t *inode, uint64_t size)
{
    char fatfs_path[VFS_PATH_MAX + 4];
    int pret = fatfs_inode_fatfs_path(inode, fatfs_path, sizeof(fatfs_path));
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }

    FIL fil;
    FRESULT fr = f_open(&fil, fatfs_path, FA_WRITE | FA_OPEN_EXISTING);
    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }

    /* f_truncate 只能截断到当前文件指针位置，无法直接传入目标大小，因此先 f_lseek 再 f_truncate */
    fr = f_lseek(&fil, (DWORD)size);
    if (fr == FR_OK)
    {
        fr = f_truncate(&fil);
    }

    f_close(&fil);

    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }

    inode->i_size = size;
    return ENO0_NO_ERROR;
}

static inode_operations_t fatfs_inode_ops = {
    .create   = fatfs_create_cb,
    .lookup   = fatfs_lookup_cb,
    .unlink   = fatfs_unlink_cb,
    .mkdir    = fatfs_mkdir_cb,
    .rmdir    = fatfs_rmdir_cb,
    .rename   = fatfs_rename_cb,
    .truncate = fatfs_truncate_cb,
};

/* ============================================================
 * 文件操作回调实现
 * ============================================================ */

/**
 * @brief 打开文件或目录
 * @param[in]     inode 文件/目录 inode
 * @param[in,out] file  文件对象（f_private 将存入 FIL* 或 fatfs_dir_priv_t*）
 * @param[in]     mode  打开模式（O_RDONLY/O_WRONLY/O_RDWR/O_CREAT 等）
 * @retval ENO0_NO_ERROR 成功
 * @note 普通文件走 f_open，把 VFS O_* 映射成 FatFS FA_*，f_private 存 FIL*；
 *   目录走 f_opendir，f_private 存 fatfs_dir_priv_t*（供 getdents64 遍历）。
 *   两条分支的 f_private 类型不同，后续所有回调都靠 S_ISDIR(f_inode->i_mode) 分流。
 *   目录的写打开已由 vfs_open 提前挡掉，这里到达时必然是只读的。
 */
static int fatfs_open_cb(inode_t *inode, file_t *file, int mode)
{
    char fatfs_path[VFS_PATH_MAX + 4];
    int pret = fatfs_inode_fatfs_path(inode, fatfs_path, sizeof(fatfs_path));
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }

    if (S_ISDIR(inode->i_mode))
    {
        fatfs_dir_priv_t *dpriv = (fatfs_dir_priv_t *)kmalloc(sizeof(fatfs_dir_priv_t));
        if (!dpriv)
        {
            return ENO1_NOMORE_MEM;
        }
        memset(dpriv, 0, sizeof(*dpriv));

        FRESULT fr = f_opendir(&dpriv->dir, fatfs_path);
        if (fr != FR_OK)
        {
            kfree(dpriv);
            return fresult_to_vfs(fr);
        }

        dpriv->synth    = FATFS_DIR_SYNTH_DOT;
        dpriv->next_off = 0;
        file->f_private = dpriv;
        file->f_op      = &fatfs_file_ops;
        return ENO0_NO_ERROR;
    }

    /* 将 VFS O_* 访问模式映射为 FatFS FA_* 标志 */
    BYTE fa = 0;
    int acc = mode & O_ACCMODE;
    if (acc == O_RDONLY || acc == O_RDWR)
    {
        fa |= FA_READ;
    }
    if (acc == O_WRONLY || acc == O_RDWR)
    {
        fa |= FA_WRITE;
    }

    if (mode & O_TRUNC)
    {
        /* FA_CREATE_ALWAYS：若文件存在则清零，若不存在则创建 */
        fa |= FA_CREATE_ALWAYS;
    }
    else if (mode & O_CREAT)
    {
        if (mode & O_EXCL)
        {
            fa |= FA_CREATE_NEW;   /* 文件已存在则失败 */
        }
        else
        {
            fa |= FA_OPEN_ALWAYS;  /* 不存在则创建，存在则打开 */
        }
    }
    else
    {
        fa |= FA_OPEN_EXISTING;    /* 文件不存在则失败 */
    }

    FIL *fil = (FIL *)slab_cache_alloc(fil_cache);
    if (!fil)
    {
        return ENO1_NOMORE_MEM;
    }

    FRESULT fr = f_open(fil, fatfs_path, fa);
    if (fr != FR_OK)
    {
        kfree(fil);
        return fresult_to_vfs(fr);
    }

    file->f_private = fil;           /* 将 FIL* 存入文件对象私有数据 */
    file->f_op      = &fatfs_file_ops;
    return ENO0_NO_ERROR;
}

/**
 * @brief 关闭文件或目录
 * @param[in] file 文件对象（f_private 中的 FIL* / fatfs_dir_priv_t* 将被关闭并释放）
 * @retval ENO0_NO_ERROR 成功
 */
static int fatfs_close_cb(file_t *file)
{
    if (!file->f_private)
    {
        return ENO0_NO_ERROR;
    }

    if (file->f_inode && S_ISDIR(file->f_inode->i_mode))
    {
        fatfs_dir_priv_t *dpriv = (fatfs_dir_priv_t *)file->f_private;
        f_closedir(&dpriv->dir);
        kfree(dpriv);
        file->f_private = NULL;
        return ENO0_NO_ERROR;
    }

    FIL *fil = (FIL *)file->f_private;
    f_close(fil);
    kfree(fil);
    file->f_private = NULL;
    return ENO0_NO_ERROR;
}

/**
 * @brief 从文件中读取数据
 * @param[in]  file 文件对象
 * @param[out] buf  接收数据的缓冲区
 * @param[in]  len  请求读取的字节数
 * @return 实际读取字节数；负值表示错误码
 */
static ssize_t fatfs_read_cb(file_t *file, void *buf, size_t len)
{
    /* 目录的 f_private 是 fatfs_dir_priv_t* 而不是 FIL*，误当 FIL* 用会写坏内存。
     * 目录必须走 getdents64（readdir 回调），read() 一律拒绝，与 Linux 语义一致。 */
    if (file->f_inode && S_ISDIR(file->f_inode->i_mode))
    {
        return (ssize_t)ENO10_IS_DIR;
    }

    FIL *fil = (FIL *)file->f_private;
    if (!fil)
    {
        return (ssize_t)ENO8_NULL_POINTER;
    }

    UINT br = 0;
    /* 参数：文件句柄 | 目标缓冲区 | 要读的长度 | 实际读取长度 */
    FRESULT fr = f_read(fil, buf, (UINT)len, &br);
    if (fr != FR_OK)
    {
        return (ssize_t)fresult_to_vfs(fr);
    }

    /* 同步 VFS 层的读写偏移量，方便下次读写从这里继续 */
    file->f_pos = (off_t)f_tell(fil);
    return (ssize_t)br;
}

/**
 * @brief 向文件中写入数据
 * @param[in] file 文件对象
 * @param[in] buf  待写入数据缓冲区
 * @param[in] len  写入字节数
 * @return 实际写入字节数；负值表示错误码
 * @note 写入后自动更新 file->f_pos 和 inode->i_size。
 */
static ssize_t fatfs_write_cb(file_t *file, const void *buf, size_t len)
{
    /* 同 fatfs_read_cb：目录的 f_private 类型不同，且写目录本就非法 */
    if (file->f_inode && S_ISDIR(file->f_inode->i_mode))
    {
        return (ssize_t)ENO10_IS_DIR;
    }

    FIL *fil = (FIL *)file->f_private;
    if (!fil)
    {
        return (ssize_t)ENO8_NULL_POINTER;
    }

    UINT bw = 0;
    FRESULT fr = f_write(fil, buf, (UINT)len, &bw);
    if (fr != FR_OK)
    {
        return (ssize_t)fresult_to_vfs(fr);
    }

    file->f_pos = (off_t)f_tell(fil);

    /* 同步更新 inode 中记录的文件大小 */
    if (file->f_pos > (off_t)file->f_inode->i_size)
    {
        /* write只负责文件长度不变（覆盖）或变大的情况，变小由truncate处理 */
        file->f_inode->i_size = (uint64_t)file->f_pos;
    }

    return (ssize_t)bw;
}

/**
 * @brief 移动文件读写位置
 * @param[in] file   文件对象
 * @param[in] offset 偏移量
 * @param[in] whence 参照点（SEEK_SET / SEEK_CUR / SEEK_END）
 * @return 新的文件位置；负值表示错误码
 */
static off_t fatfs_lseek_cb(file_t *file, off_t offset, int whence)
{
    /* 目录：只支持 lseek(fd, 0, SEEK_SET) —— 等价于 rewinddir，把 FatFS 游标、
     * "."/".." 合成阶段机、pending 暂存三者一起复位。其余组合在目录上没有
     * 有意义的语义（记录是变长的，任意 offset 无法定位到记录边界），返回 EINVAL。 */
    if (file->f_inode && S_ISDIR(file->f_inode->i_mode))
    {
        fatfs_dir_priv_t *dpriv = (fatfs_dir_priv_t *)file->f_private;
        if (!dpriv)
        {
            return (off_t)ENO8_NULL_POINTER;
        }
        if (whence != SEEK_SET || offset != 0)
        {
            return (off_t)ENO6_INVAL_PARAM;
        }

        FRESULT fr = f_rewinddir(&dpriv->dir);
        if (fr != FR_OK)
        {
            return (off_t)fresult_to_vfs(fr);
        }
        dpriv->synth       = FATFS_DIR_SYNTH_DOT;
        dpriv->has_pending = false;
        dpriv->next_off    = 0;
        file->f_pos        = 0;
        return 0;
    }

    FIL *fil = (FIL *)file->f_private;
    if (!fil)
    {
        return (off_t)ENO8_NULL_POINTER;
    }

    DWORD new_pos;
    if (whence == SEEK_SET) /* 从文件开头跳 offset 字节 */
    {
        new_pos = (DWORD)offset;
    }
    else if (whence == SEEK_CUR) /* 从当前位置跳 offset 字节 */
    {
        new_pos = (DWORD)((off_t)f_tell(fil) + offset);
    }
    else if (whence == SEEK_END) /* 从文件末尾跳 offset 字节 */
    {
        new_pos = (DWORD)((off_t)f_size(fil) + offset);
    }
    else
    {
        return (off_t)ENO6_INVAL_PARAM;
    }

    FRESULT fr = f_lseek(fil, new_pos);
    if (fr != FR_OK)
    {
        return (off_t)fresult_to_vfs(fr);
    }

    file->f_pos = (off_t)f_tell(fil);
    return file->f_pos;
}

/**
 * @brief 把一条目录项按 struct linux_dirent64 布局写进缓冲区
 * @param[out] buf     目标位置（调用方已确保剩余空间 >= reclen）
 * @param[in]  name    条目名（'\0' 结尾）
 * @param[in]  is_dir  true = 目录（DT_DIR），false = 普通文件（DT_REG）
 * @param[in]  off     写入 d_off 的游标值（"下一条记录的位置"，我们用已返回条目序号充当）
 * @param[in]  reclen  本条记录总长度（已 8 字节对齐）
 * @note `d_ino` 必须非 0——FAT 没有 inode 号，而某些程序会把 d_ino==0 当作
 *   "已删除条目"跳过，所以用 off+1 充当一个目录内唯一且非零的值。
 */
static void fatfs_fill_dirent(void *buf, const char *name, bool is_dir,
                              uint64_t off, uint16_t reclen)
{
    struct linux_dirent64 *de = (struct linux_dirent64 *)buf;
    size_t namelen = strlen(name);

    /* 记录尾部的对齐填充字节要清零：这块内存直接 copy_to_user 给用户态，
     * 不清零等于把内核堆里的残留数据泄漏出去 */
    memset(buf, 0, reclen);

    de->d_ino    = off + 1;
    de->d_off    = (int64_t)(off + 1);
    de->d_reclen = reclen;
    de->d_type   = is_dir ? DT_DIR : DT_REG;
    memcpy(de->d_name, name, namelen + 1);
}

/* 一条目录项记录所需的总字节数：头部 + 名字 + '\0'，向上取整到 8 字节。
 * d_off 是 8 字节字段，下一条记录必须对齐到 8 才能被安全访问。 */
static uint16_t fatfs_dirent_reclen(const char *name)
{
    size_t need = offsetof(struct linux_dirent64, d_name) + strlen(name) + 1;
    return (uint16_t)((need + 7) & ~(size_t)7);
}

/**
 * @brief 读取目录项，填充紧凑排列的 struct linux_dirent64 记录（getdents64 后端）
 * @param[in]  file 已打开的目录 file
 * @param[out] buf  目标缓冲区
 * @param[in]  len  缓冲区容量
 * @retval >0 已填字节数
 * @retval 0  目录读完
 * @retval ENO6_INVAL_PARAM 缓冲区连一条记录都放不下
 * @details 每轮先确定"下一条要吐的条目"，来源有三种，优先级从高到低：
 *   1. `pending`——上次因缓冲区满而暂存的那条（它已被 FatFS 游标消费，不吐就永久丢失）；
 *   2. 合成的 `.` / `..`——FatFS 的 f_readdir 不会返回它们（见 FATFS_DIR_SYNTH_* 注释）；
 *   3. `f_readdir` 真实读出的条目。
 *   拿到条目后先算 reclen，放不下就停手——**一条记录绝不能被截断**。此时若该条来自
 *   f_readdir 则必须存进 pending；若来自合成阶段或本就是 pending，则保持状态不变，
 *   下次调用会重新生成同一条。
 */
static int fatfs_readdir_cb(file_t *file, void *buf, size_t len)
{
    fatfs_dir_priv_t *dpriv = (fatfs_dir_priv_t *)file->f_private;
    if (!dpriv)
    {
        return ENO8_NULL_POINTER;
    }

    size_t written = 0;

    while (1)
    {
        const char *name;
        bool        is_dir;
        bool        from_readdir = false;
        FILINFO     fno;

        if (dpriv->has_pending)
        {
            name   = dpriv->pending_name;
            is_dir = dpriv->pending_is_dir;
        }
        else if (dpriv->synth == FATFS_DIR_SYNTH_DOT)
        {
            name   = ".";
            is_dir = true;
        }
        else if (dpriv->synth == FATFS_DIR_SYNTH_DOTDOT)
        {
            name   = "..";
            is_dir = true;
        }
        else
        {
            /* _USE_LFN 开启后必须由调用方在每次 f_readdir 之前把 lfname 指向缓冲、
             * lfsize 填容量，否则拿不到长文件名（get_fileinfo 按 lfname 是否非 NULL
             * 决定要不要填）。无长名时 get_fileinfo 会把 lfname 置成空串。 */
            memset(&fno, 0, sizeof(fno));
            fno.lfname = dpriv->lfn_buf;
            fno.lfsize = sizeof(dpriv->lfn_buf);

            FRESULT fr = f_readdir(&dpriv->dir, &fno);
            if (fr != FR_OK)
            {
                return written ? (int)written : fresult_to_vfs(fr);
            }
            if (fno.fname[0] == '\0') /* FatFS 约定：空 fname 表示目录已读完 */
            {
                break;
            }

            name         = (fno.lfname[0] != '\0') ? fno.lfname : fno.fname;
            is_dir       = (fno.fattrib & AM_DIR) != 0;
            from_readdir = true;
        }

        uint16_t reclen = fatfs_dirent_reclen(name);
        if (written + reclen > len)
        {
            if (written == 0)
            {
                /* 连一条都放不下：调用方给的缓冲区太小，按 getdents64 约定报 EINVAL */
                return ENO6_INVAL_PARAM;
            }
            if (from_readdir)
            {
                /* 这条已经从 FatFS 游标里消费掉了，必须暂存，否则永久丢失 */
                size_t nlen = strlen(name);
                if (nlen > _MAX_LFN)
                {
                    nlen = _MAX_LFN;
                }
                memcpy(dpriv->pending_name, name, nlen);
                dpriv->pending_name[nlen] = '\0';
                dpriv->pending_is_dir     = is_dir;
                dpriv->has_pending        = true;
            }
            break;
        }

        fatfs_fill_dirent((char *)buf + written, name, is_dir, dpriv->next_off, reclen);
        written += reclen;
        dpriv->next_off += 1;

        if (dpriv->has_pending)
        {
            dpriv->has_pending = false;
        }
        else if (dpriv->synth < FATFS_DIR_SYNTH_DONE)
        {
            dpriv->synth += 1;
        }
    }

    file->f_pos = (off_t)dpriv->next_off;
    return (int)written;
}

static file_operations_t fatfs_file_ops = {
    .open    = fatfs_open_cb,
    .close   = fatfs_close_cb,
    .read    = fatfs_read_cb,
    .write   = fatfs_write_cb,
    .lseek   = fatfs_lseek_cb,
    .ioctl   = NULL,
    .readdir = fatfs_readdir_cb,
};

/* ============================================================
 * 文件系统类型描述符与注册函数
 * ============================================================ */

file_system_type_t fatfs_fs_type = {
    .name    = "fatfs",
    .mount   = fatfs_mount_cb,
    .kill_sb = NULL,  /* 卸载由 sb_ops.unmount 回调处理 */
    .next    = NULL,
};

/**
 * @brief 向 VFS 注册 fatfs 文件系统类型
 * @note 由 fs_init() 在 vfs_init() 之后调用。
 */
void fatfs_register(void)
{
    int16_t ret = register_filesystem(&fatfs_fs_type);
    if (ret != ENO0_NO_ERROR)
    {
        printf("fatfs_register: failed, err=%d\n", ret);
    }
}
