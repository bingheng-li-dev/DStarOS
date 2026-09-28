/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/*
 * fatfs_vfs.c - FatFS ↔ VFS 适配层
 *
 * 将 ChaN FatFS R0.11 的路径式 API 桥接到 DStarOS VFS 的四层抽象接口。
 *
 * 设计映射关系：
 *   VFS 抽象          FatFS 对应           实现方式
 *   ─────────────────────────────────────────────────────
 *   super_block_t  ←→  FatFS 卷            fatfs_volume_t*（FATFS + 卷前缀）存入 sb->s_private
 *   inode_t        ←→  路径字符串          由 fatfs_build_path() 沿 dentry 链现推（不缓存）
 *   dentry_t       ←→  路径分量            由 VFS dentry_create 管理
 *   file_t         ←→  FIL 对象            FIL* 存入 file->f_private
 *
 * 0 号卷（RAM 盘）挂载时若无有效 FAT 则自动格式化；SD 卡不会。
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

/* inode 不持有私有数据（i_private 恒为 NULL）：卷内路径由 fatfs_build_path() 沿
 * d_parent 现推、不缓存——缓存的路径在 rename 后会让整棵子树失效。 */

/* 合成阶段机的取值：FatFS 的 f_readdir 会跳过 FAT 目录里真实存在的 "." / ".." 项
 * （ff.c 的 dir_read 在 _FS_RPATH=0 时过滤掉所有以 '.' 开头的条目），而
 * BusyBox 的 ls -a / rm -r / find 都期待它们出现，只能由适配层自己补上。 */
#define FATFS_DIR_SYNTH_DOT    0  /* 待吐 "." */
#define FATFS_DIR_SYNTH_DOTDOT 1  /* 待吐 ".." */
#define FATFS_DIR_SYNTH_DONE   2  /* 两条都吐完了，进入 f_readdir 循环 */

/* 打开的目录 file 的私有数据。普通文件的 f_private 是 FIL*，目录是本结构，靠
 * S_ISDIR(f_inode->i_mode) 区分；vfs_open 调 f_op->open 前已填好 f_inode，不会混淆。 */
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

/* 每个 FatFS 卷一份，挂在 sb->s_private：f_mount 要求 FATFS 对象持久存在直至卸载，
 * 卷前缀 "N:" 决定一次 FatFS 调用落在哪个物理驱动器上（0 = RAM 盘，1 = SD 卡）。 */
typedef struct fatfs_volume
{
    FATFS fs;
    char  prefix[3];
} fatfs_volume_t;

static super_block_operations_t fatfs_sb_ops;
static inode_operations_t       fatfs_inode_ops;
static file_operations_t        fatfs_file_ops;

/* ============================================================
 * FatFS 内存分配钩子（_USE_LFN=3 需要，见 ffconf.h）
 * ============================================================ */

/**
 * @brief FatFS 内存分配钩子（_USE_LFN=3 需要）
 */
void *ff_memalloc(UINT msize)
{
    return kmalloc(msize);
}

/**
 * @brief FatFS 内存释放钩子
 */
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
 * @note 仓库未附带 437 码页转换表（ChaN 的 option/cc437.c）。ASCII（<0x80）在 OEM 与
 *   Unicode 下逐字节相同，原样返回；0x80 以上按无法转换处理。已知限制：非 ASCII
 *   文件名会被拒绝创建或显示为 '?'。
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

/* 卷内路径 → FatFS 路径："" → "N:/"，"/dir/f" → "N:/dir/f"；缓冲区不够时输出空串 */
static void vfs_to_fatfs_path(const super_block_t *sb, const char *vfs_path, char *buf, int bufsz)
{
    const fatfs_volume_t *vol = (const fatfs_volume_t *)sb->s_private;
    int plen = (vfs_path != NULL) ? (int)strlen(vfs_path) : 0;

    if (2 + (plen > 0 ? plen : 1) + 1 > bufsz)
    {
        buf[0] = '\0';
        return;
    }
    buf[0] = vol->prefix[0];
    buf[1] = ':';
    if (plen == 0)
    {
        buf[2] = '/';
        buf[3] = '\0';
    }
    else
    {
        memcpy(buf + 2, vfs_path, plen + 1);
    }
}

/* parent_path + "/" + name；父为根（""）时得 "/name"，缓冲区不够时输出空串 */
static void fatfs_make_child_path(const char *parent_path,
                                   const char *name,
                                   char *buf, int bufsz)
{
    int plen = (int)strlen(parent_path);
    int nlen = (int)strlen(name);

    if (plen == 0)
    {
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
 * @param[out] sb_out 非 NULL 时写入该目录项所在卷的超级块（取自卷根 inode）
 * @retval ENO0_NO_ERROR     成功；卷根本身得到空字符串 ""（与 vfs_to_fatfs_path 的约定一致）
 * @retval ENO5_NOSUCH_ENTRY 这条链上有目录项已被 unlink/rmdir 脱链，路径无意义
 * @retval ENO11_NAME_TOO_LONG 拼出来超过 bufsz
 * @details 倒着往缓冲区尾部写再整体前移，免掉一个用来反转分量顺序的辅助栈——
 *   内核栈只有一页，递归或变长数组都不合适。
 * @note 终点必须是本文件系统的根 inode。脱链的目录项同样以"d_parent 指向自身"
 *   标记，光看循环退出条件区分不了，不检查的话会把一条张冠李戴的路径交给 FatFS。
 */
static int fatfs_build_path(const dentry_t *d, char *buf, int bufsz, super_block_t **sb_out)
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

    if (sb_out != NULL)
    {
        *sb_out = d->d_inode->i_sb;
    }
    memmove(buf, buf + pos, bufsz - pos);
    return ENO0_NO_ERROR;
}

/* inode 对应的 FatFS 卷路径（"N:/dir/f"），buf 容量需 >= VFS_PATH_MAX + 4 */
static int fatfs_inode_fatfs_path(const inode_t *inode, char *buf, int bufsz)
{
    char vfs_path[VFS_PATH_MAX];

    if (inode == NULL)
    {
        return ENO8_NULL_POINTER;
    }
    super_block_t *sb = NULL;
    int ret = fatfs_build_path(inode->i_dentry, vfs_path, sizeof(vfs_path), &sb);
    if (ret != ENO0_NO_ERROR)
    {
        return ret;
    }
    vfs_to_fatfs_path(sb, vfs_path, buf, bufsz);
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

/* 分配并基本初始化一个 fatfs inode；i_mode / i_size 由调用者填 */
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
    kfree(inode);
}

static int fatfs_sync_fs_cb(super_block_t *sb)
{
    (void)sb;
    /* FatFS 写操作已即时同步到 ramdisk/SD 卡，无需额外操作 */
    return ENO0_NO_ERROR;
}

static int fatfs_unmount_cb(super_block_t *sb)
{
    fatfs_volume_t *vol = (fatfs_volume_t *)sb->s_private;
    f_mount(NULL, vol->prefix, 0);
    kfree(vol); /* destroy_super_block 只释放超级块本身 */
    sb->s_private = NULL;
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

/* 卷号字符串 → 物理驱动器号；NULL 表示 0 号 */
static bool fatfs_parse_volume(const char *s, BYTE *pdrv)
{
    *pdrv = 0;
    if (s == NULL)
    {
        return true;
    }
    if (s[0] < '0' || s[0] >= '0' + _VOLUMES || s[1] != '\0')
    {
        printf("fatfs_mount: bad volume \"%s\"\n", s);
        return false;
    }
    *pdrv = (BYTE)(s[0] - '0');
    return true;
}

/* 挂上卷并读取 BPB；只有 0 号卷读不到 FAT 时先格式化再挂。失败时已打印原因 */
static bool fatfs_mount_volume(fatfs_volume_t *vol, BYTE pdrv)
{
    FRESULT fr = f_mount(&vol->fs, vol->prefix, 1);
    if (fr == FR_NO_FILESYSTEM && pdrv == 0)
    {
        printf("fatfs_mount: no filesystem, formatting ramdisk...\n");
        BYTE work[512];
        fr = f_mkfs(vol->prefix, 0, sizeof(work));
        if (fr != FR_OK)
        {
            printf("fatfs_mount: f_mkfs failed, fr=%d\n", (int)fr);
            return false;
        }
        fr = f_mount(&vol->fs, vol->prefix, 1);
    }
    if (fr != FR_OK)
    {
        printf("fatfs_mount: disk %u f_mount failed, fr=%d\n", (unsigned)pdrv, (int)fr);
        return false;
    }
    return true;
}

/**
 * @brief FatFS 挂载回调入口
 * @param[in] data 卷号字符串，"1" 表示 1 号物理驱动器（VF2 的 SD 卡）；NULL 表示 0 号（RAM 盘）
 * @return 根目录的 dentry 指针；失败返回 NULL
 * @details 只有 0 号卷允许自动格式化：RAM 盘没装镜像时本来就是一片零，格式化是预期行为；
 *   对 SD 卡，"读不到 FAT"更可能是驱动或寻址出了错，此时 f_mkfs 等于亲手抹掉整张卡。
 * @note f_mount(opt=1) 失败时 FatFS 已把 FATFS 指针登记进卷表，释放前必须先注销。
 */
static dentry_t *fatfs_mount_cb(file_system_type_t *fst,
                                 const char *source,
                                 void *data)
{
    (void)fst;
    (void)source;

    BYTE pdrv;
    if (!fatfs_parse_volume((const char *)data, &pdrv))
    {
        return NULL;
    }

    fatfs_volume_t *vol = (fatfs_volume_t *)kmalloc(sizeof(fatfs_volume_t));
    if (!vol)
    {
        return NULL;
    }
    memset(vol, 0, sizeof(*vol));
    vol->prefix[0] = (char)('0' + pdrv);
    vol->prefix[1] = ':';
    vol->prefix[2] = '\0';

    DSTATUS dstat = disk_initialize(pdrv);
    if (dstat & STA_NOINIT)
    {
        printf("fatfs_mount: disk %u init failed, dstat=0x%x\n", (unsigned)pdrv, (unsigned)dstat);
        kfree(vol);
        return NULL;
    }

    super_block_t *sb = NULL;
    inode_t *root_inode = NULL;
    if (!fatfs_mount_volume(vol, pdrv))
    {
        goto fail_mount;
    }

    /* 拿不到扇区数（SD 卡只读挂载不提供）时记 0，只影响 statfs 类信息 */
    DWORD sector_count = 0;
    if (disk_ioctl(pdrv, GET_SECTOR_COUNT, &sector_count) != RES_OK)
    {
        sector_count = 0;
    }
    sb = alloc_super_block("fatfs", 512, (uint64_t)sector_count, &fatfs_sb_ops, vol);
    if (!sb)
    {
        goto fail_mount;
    }

    root_inode = fatfs_alloc_inode_internal(sb);
    if (!root_inode)
    {
        goto fail_sb;
    }
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_size = 0;
    /* 卷根的路径是空字符串，由 fatfs_build_path 在循环一次不跑时自然得到 */

    /* parent=NULL：文件系统局部根，d_parent 指向自身 */
    dentry_t *root_dentry = dentry_create("", root_inode, NULL, NULL);
    if (!root_dentry)
    {
        goto fail_inode;
    }
    root_inode->i_dentry = root_dentry;
    sb->s_root_inode     = root_inode;

    if (pdrv == 0)
    {
        printf("fatfs_mount: mounted ok (%u sectors)\n", (unsigned)sector_count);
    }
    else
    {
        printf("fatfs_mount: disk %u mounted ok%s\n", (unsigned)pdrv,
               (dstat & STA_PROTECT) ? " (read-only)" : "");
    }
    return root_dentry;

fail_inode:
    fatfs_destroy_inode_cb(root_inode);
fail_sb:
    destroy_super_block(sb);
fail_mount:
    f_mount(NULL, vol->prefix, 0);
    kfree(vol);
    return NULL;
}

/* ============================================================
 * inode 操作回调实现
 * ============================================================ */

/* 查 dir 下的 name：f_stat 存在则建 inode + dentry 进缓存，返回持引用的 dentry，未找到返回 NULL */
static dentry_t *fatfs_lookup_cb(inode_t *dir, const char *name)
{
    char dir_vfs[VFS_PATH_MAX];
    char child_vfs[VFS_PATH_MAX];
    char child_fatfs[VFS_PATH_MAX + 4];
    super_block_t *sb = NULL;
    if (fatfs_build_path(dir->i_dentry, dir_vfs, sizeof(dir_vfs), &sb) != ENO0_NO_ERROR)
    {
        return NULL;
    }
    fatfs_make_child_path(dir_vfs, name, child_vfs, VFS_PATH_MAX);
    vfs_to_fatfs_path(sb, child_vfs, child_fatfs, sizeof(child_fatfs));

    /* _USE_LFN 下 f_stat 看 lfname 非空就往里写长文件名；这里用不到长名，清零使
     * lfname=NULL，避免栈上未初始化的指针被写入。 */
    FILINFO finfo;
    memset(&finfo, 0, sizeof(finfo));
    FRESULT fr = f_stat(child_fatfs, &finfo);
    if (fr != FR_OK)
    {
        return NULL;
    }

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

    dentry_t *d = dentry_create(name, inode, dir->i_dentry, NULL);
    if (!d)
    {
        fatfs_destroy_inode_cb(inode);
        return NULL;
    }
    inode->i_dentry = d;

    return d;
}

/* 在磁盘上创建普通文件并填好负目录项；mode 忽略，固定 S_IFREG|0755 */
static int fatfs_create_cb(inode_t *dir, dentry_t *dentry, mode_t mode)
{
    (void)mode;

    /* dentry 此刻已由 dentry_create 挂进 dir 的子链表，直接从它上溯即可 */
    char child_vfs[VFS_PATH_MAX];
    char child_fatfs[VFS_PATH_MAX + 4];
    super_block_t *sb = NULL;
    int pret = fatfs_build_path(dentry, child_vfs, sizeof(child_vfs), &sb);
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(sb, child_vfs, child_fatfs, sizeof(child_fatfs));

    FIL fil;
    FRESULT fr = f_open(&fil, child_fatfs, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }
    f_close(&fil);

    inode_t *inode = fatfs_alloc_inode_internal(dir->i_sb);
    if (!inode)
    {
        return ENO1_NOMORE_MEM;
    }

    /* 可执行位的理由见 fatfs_lookup_cb；新建文件不会带 FAT 只读属性 */
    inode->i_mode   = S_IFREG | 0755;
    inode->i_size   = 0;
    inode->i_dentry = dentry;
    dentry->d_inode = inode;

    return ENO0_NO_ERROR;
}

/* 在磁盘上创建目录并填好负目录项；mode 忽略，固定 S_IFDIR|0755 */
static int fatfs_mkdir_cb(inode_t *dir, dentry_t *dentry, mode_t mode)
{
    (void)mode;

    char child_vfs[VFS_PATH_MAX];
    char child_fatfs[VFS_PATH_MAX + 4];
    super_block_t *sb = NULL;
    int pret = fatfs_build_path(dentry, child_vfs, sizeof(child_vfs), &sb);
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(sb, child_vfs, child_fatfs, sizeof(child_fatfs));

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

/* 删除文件；f_unlink 对文件和空目录均适用 */
static int fatfs_unlink_cb(inode_t *dir, dentry_t *dentry)
{
    (void)dir;

    char vfs_path[VFS_PATH_MAX];
    char fatfs_path[VFS_PATH_MAX + 4];
    super_block_t *sb = NULL;
    int pret = fatfs_build_path(dentry, vfs_path, sizeof(vfs_path), &sb);
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(sb, vfs_path, fatfs_path, sizeof(fatfs_path));

    return fresult_to_vfs(f_unlink(fatfs_path));
}

/* 删除空目录，委托给 fatfs_unlink_cb */
static int fatfs_rmdir_cb(inode_t *dir, dentry_t *dentry)
{
    return fatfs_unlink_cb(dir, dentry);
}

/* 重命名 / 移动文件或目录 */
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

    super_block_t *old_sb = NULL;
    super_block_t *new_sb = NULL;
    int pret = fatfs_build_path(old_dentry, old_vfs, sizeof(old_vfs), &old_sb);
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    pret = fatfs_build_path(new_dentry, new_vfs, sizeof(new_vfs), &new_sb);
    if (pret != ENO0_NO_ERROR)
    {
        return pret;
    }
    vfs_to_fatfs_path(old_sb, old_vfs, old_fatfs, sizeof(old_fatfs));
    vfs_to_fatfs_path(new_sb, new_vfs, new_fatfs, sizeof(new_fatfs));

    FRESULT fr = f_rename(old_fatfs, new_fatfs);
    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }

    /* 路径不缓存：vfs_rename 把 dentry 挂到新父目录后，整棵子树的路径自然跟着变 */
    return ENO0_NO_ERROR;
}

/* 截断或扩展文件到 size，成功后更新 i_size。
 * FAT32 文件大小上限为 4 GB（DWORD 限制）。 */
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

    file->f_private = fil;
    file->f_op      = &fatfs_file_ops;
    return ENO0_NO_ERROR;
}

/* 关闭文件或目录，释放 f_private 里的 FIL* / fatfs_dir_priv_t* */
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
 * @brief 把 FatFS 的 FIL 内部读写指针对齐到 VFS 的 file->f_pos
 * @retval ENO0_NO_ERROR 已对齐（本来就相等，或 f_lseek 成功）
 * @details FIL 自带一个读写指针，而 VFS 层会在不经过 f_op->lseek 的情况下改
 *   file->f_pos——O_APPEND 就是这么干的：vfs_open 把 f_pos 置成 i_size，vfs_write
 *   每次写前再置一次。两边不同步的话，写会落在 FIL 指针所在的位置上。
 *   顺序读写与 lseek 时两者天然同步。
 */
static int fatfs_sync_pos(file_t *file, FIL *fil)
{
    if ((off_t)f_tell(fil) == file->f_pos)
    {
        return ENO0_NO_ERROR;
    }
    FRESULT fr = f_lseek(fil, (DWORD)file->f_pos);
    if (fr != FR_OK)
    {
        return fresult_to_vfs(fr);
    }
    return ENO0_NO_ERROR;
}

/* 读文件；目录一律拒绝（须走 readdir） */
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

    int sret = fatfs_sync_pos(file, fil);
    if (sret != ENO0_NO_ERROR)
    {
        return (ssize_t)sret;
    }

    UINT br = 0;
    FRESULT fr = f_read(fil, buf, (UINT)len, &br);
    if (fr != FR_OK)
    {
        return (ssize_t)fresult_to_vfs(fr);
    }

    file->f_pos = (off_t)f_tell(fil);
    return (ssize_t)br;
}

/* 写文件，并同步 f_pos 与 i_size */
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

    int sret = fatfs_sync_pos(file, fil);
    if (sret != ENO0_NO_ERROR)
    {
        return (ssize_t)sret;
    }

    UINT bw = 0;
    FRESULT fr = f_write(fil, buf, (UINT)len, &bw);
    if (fr != FR_OK)
    {
        return (ssize_t)fresult_to_vfs(fr);
    }

    file->f_pos = (off_t)f_tell(fil);

    if (file->f_pos > (off_t)file->f_inode->i_size)
    {
        /* write只负责文件长度不变（覆盖）或变大的情况，变小由truncate处理 */
        file->f_inode->i_size = (uint64_t)file->f_pos;
    }

    return (ssize_t)bw;
}

/* 移动读写位置；目录只支持 rewind，见函数体 */
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
    if (whence == SEEK_SET)
    {
        new_pos = (DWORD)offset;
    }
    else if (whence == SEEK_CUR)
    {
        new_pos = (DWORD)((off_t)f_tell(fil) + offset);
    }
    else if (whence == SEEK_END)
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

/* 按 linux_dirent64 布局写一条记录，off 为已返回条目序号。d_ino 必须非 0（有程序把
 * d_ino==0 当已删除条目跳过），FAT 没有 inode 号，用 off+1 充当。 */
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

/* 缓冲区放不下时暂存这条：它已经从 FatFS 游标里消费掉了，不存就永久丢失 */
static void fatfs_stash_pending(fatfs_dir_priv_t *dpriv, const char *name, bool is_dir)
{
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

/**
 * @brief 读取目录项，填充紧凑排列的 struct linux_dirent64 记录（getdents64 后端）
 * @retval >0 已填字节数
 * @retval 0  目录读完
 * @retval ENO6_INVAL_PARAM 缓冲区连一条记录都放不下
 * @details 每轮先确定"下一条要吐的条目"，来源有三种，优先级从高到低：
 *   1. `pending`——上次因缓冲区满而暂存的那条（它已被 FatFS 游标消费，不吐就永久丢失）；
 *   2. 合成的 `.` / `..`——FatFS 的 f_readdir 不会返回它们（见 FATFS_DIR_SYNTH_* 注释）；
 *   3. `f_readdir` 真实读出的条目。
 *   拿到条目后先算 reclen，放不下就停手——一条记录绝不能被截断。此时若该条来自
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
                fatfs_stash_pending(dpriv, name, is_dir);
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

static file_system_type_t fatfs_fs_type = {
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
