/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "devfs.h"
#include "slab.h"
#include "vfs.h"
#include "tty.h"
#include "kmalloc.h"
#include "stringops.h"
#include "errorcode.h"
#include "linux_abi.h"
#include "console.h"

/* devfs：FAT 存不下设备节点，所以造一个不落盘的合成文件系统挂在 /dev。
 * 设备集合固定（4 个），挂载时一次建好全部子 dentry，vfs_lookup 走缓存命中路径
 * 就能找到，不实现 i_op->lookup；运行时注册/注销设备留待有需求时再做。 */

/* ============================================================
 * /dev/null、/dev/zero
 * ============================================================ */

static ssize_t devnull_read(file_t *f, void *buf, size_t len)
{
    (void)f;
    (void)buf;
    (void)len;
    return 0;
}

static ssize_t devnull_write(file_t *f, const void *buf, size_t len)
{
    (void)f;
    (void)buf;
    return (ssize_t)len;
}

static ssize_t devzero_read(file_t *f, void *buf, size_t len)
{
    (void)f;
    memset(buf, 0, len);
    return (ssize_t)len;
}

/* vfs_open() 把 f_kind 一律设成 FILE_KIND_VFS，这里改标 FILE_KIND_DEVICE：devfs 的
 * inode 不维护 i_size，按普通文件处理会让 lseek(SEEK_END) 读到无意义的值。 */
static int devnull_open(inode_t *inode, file_t *file, int mode)
{
    (void)inode;
    (void)mode;
    file->f_kind = FILE_KIND_DEVICE;
    return ENO0_NO_ERROR;
}

static file_operations_t devnull_fops = {
    .read = devnull_read,
    .write = devnull_write,
    .open = devnull_open,
};

static file_operations_t devzero_fops = {
    .read = devzero_read,
    .write = devnull_write, /* 写 /dev/zero 与写 /dev/null 行为相同：收下即丢 */
    .open = devnull_open,
};

/* ============================================================
 * /dev 根目录：readdir（"ls /dev"）
 * ============================================================ */

typedef struct devfs_entry
{
    const char *name;
    uint8_t     d_type;
} devfs_entry_t;

static const devfs_entry_t devfs_entries[] = {
    { "console", DT_CHR },
    { "tty",     DT_CHR },
    { "null",    DT_CHR },
    { "zero",    DT_CHR },
};
#define DEVFS_ENTRY_COUNT (sizeof(devfs_entries) / sizeof(devfs_entries[0]))

/* 每次 open("/dev") 独立的读游标，放在 f_private：next_index 0/1 是合成的 "."/".."，
 * 2.. 对应 devfs_entries[]。两个进程同时 opendir("/dev") 必须各有游标。 */
typedef struct devfs_dir_priv
{
    size_t next_index;
} devfs_dir_priv_t;

/* 与 fatfs_vfs.c 的 fatfs_fill_dirent 同一布局；那边按 is_dir 填，这里按 d_type 填，故不共用 */
static void devfs_fill_dirent(void *buf, const char *name, uint8_t d_type,
                              uint64_t off, uint16_t reclen)
{
    struct linux_dirent64 *de = (struct linux_dirent64 *)buf;
    size_t namelen = strlen(name);

    memset(buf, 0, reclen);
    de->d_ino    = off + 1; /* d_ino 不能是 0，devfs 没有真实 inode 号，用序号充当 */
    de->d_off    = (int64_t)(off + 1);
    de->d_reclen = reclen;
    de->d_type   = d_type;
    memcpy(de->d_name, name, namelen + 1);
}

static uint16_t devfs_dirent_reclen(const char *name)
{
    size_t need = offsetof(struct linux_dirent64, d_name) + strlen(name) + 1;
    return (uint16_t)((need + 7) & ~(size_t)7);
}

static int devfs_dir_open(inode_t *inode, file_t *file, int mode)
{
    (void)inode;
    (void)mode;
    devfs_dir_priv_t *priv = kmalloc(sizeof(devfs_dir_priv_t));
    if (!priv)
    {
        return ENO1_NOMORE_MEM;
    }
    priv->next_index = 0;
    file->f_private = priv;
    return ENO0_NO_ERROR;
}

static int devfs_dir_close(file_t *file)
{
    kfree(file->f_private);
    return ENO0_NO_ERROR;
}

static int devfs_dir_readdir(file_t *file, void *buf, size_t len)
{
    devfs_dir_priv_t *priv = (devfs_dir_priv_t *)file->f_private;
    if (!priv)
    {
        return ENO8_NULL_POINTER;
    }

    size_t written = 0;
    while (priv->next_index < 2 + DEVFS_ENTRY_COUNT)
    {
        const char *name;
        uint8_t     d_type;
        if (priv->next_index == 0)
        {
            name = ".";
            d_type = DT_DIR;
        }
        else if (priv->next_index == 1)
        {
            name = "..";
            d_type = DT_DIR;
        }
        else
        {
            const devfs_entry_t *e = &devfs_entries[priv->next_index - 2];
            name = e->name;
            d_type = e->d_type;
        }

        uint16_t reclen = devfs_dirent_reclen(name);
        if (written + reclen > len)
        {
            /* 连一条都放不下：调用方缓冲区太小，按 getdents64 约定报 EINVAL；
             * 已经写了至少一条则先把这些交回去，游标停在这条，下次继续 */
            if (written == 0)
            {
                return ENO6_INVAL_PARAM;
            }
            break;
        }

        devfs_fill_dirent((char *)buf + written, name, d_type, priv->next_index, reclen);
        written += reclen;
        priv->next_index++;
    }

    return (int)written;
}

static file_operations_t devfs_dir_fops = {
    .open = devfs_dir_open,
    .close = devfs_dir_close,
    .readdir = devfs_dir_readdir,
};

/* ============================================================
 * 挂载
 * ============================================================ */

static dentry_t *devfs_mount_cb(file_system_type_t *fst, const char *source, void *data)
{
    (void)fst;
    (void)source;
    (void)data;

    super_block_t *sb = alloc_super_block("devfs", 0, 0, NULL, NULL);
    if (!sb)
    {
        return NULL;
    }

    inode_t *root_inode = (inode_t *)slab_cache_alloc(inode_cache);
    if (!root_inode)
    {
        /* sb 不回收：devfs 只在启动时挂一次，失败即配置错误 */
        return NULL;
    }
    memset(root_inode, 0, sizeof(inode_t));
    root_inode->i_sb   = sb;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_fop  = &devfs_dir_fops;

    dentry_t *root_dentry = dentry_create("", root_inode, NULL, NULL);
    if (!root_dentry)
    {
        return NULL;
    }
    root_inode->i_dentry = root_dentry;
    sb->s_root_inode     = root_inode;

    /* /dev/console、/dev/tty 复用同一张 tty_fops——tty_from_file() 靠指针比对
     * 判断"是不是真正的 TTY"，必须是同一张表，见 tty.h 里 tty_fops 的注释。 */
    static const struct
    {
        const char         *name;
        file_operations_t  *fops;
    } devices[DEVFS_ENTRY_COUNT] = {
        { "console", &tty_fops },
        { "tty",     &tty_fops },
        { "null",    &devnull_fops },
        { "zero",    &devzero_fops },
    };

    for (size_t i = 0; i < DEVFS_ENTRY_COUNT; i++)
    {
        inode_t *inode = (inode_t *)slab_cache_alloc(inode_cache);
        if (!inode)
        {
            return NULL;
        }
        memset(inode, 0, sizeof(inode_t));
        inode->i_sb   = sb;
        inode->i_mode = S_IFCHR | 0620;
        inode->i_fop  = devices[i].fops;

        dentry_t *d = dentry_create(devices[i].name, inode, root_dentry, NULL);
        if (!d)
        {
            return NULL;
        }
        inode->i_dentry = d;
    }

    printf("devfs: mounted (console, tty, null, zero)\n");
    return root_dentry;
}

static file_system_type_t devfs_fs_type = {
    .name    = "devfs",
    .mount   = devfs_mount_cb,
    .kill_sb = NULL,
    .next    = NULL,
};

/**
 * @brief 注册 devfs 文件系统类型，须早于 vfs_mount("/dev", "devfs", NULL)
 */
void devfs_register(void)
{
    int16_t ret = register_filesystem(&devfs_fs_type);
    if (ret != ENO0_NO_ERROR)
    {
        printf("devfs_register: failed, err=%d\n", ret);
    }
}
