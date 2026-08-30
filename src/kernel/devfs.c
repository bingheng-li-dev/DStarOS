#include "devfs.h"
#include "slab.h"
#include "vfs.h"
#include "tty.h"
#include "kmalloc.h"
#include "stringops.h"
#include "errorcode.h"
#include "linux_abi.h"
#include "console.h"

/* devfs：给 FAT 根文件系统打的补丁——FAT 存不下设备节点，这不是 VFS 的限制，
 * 是这一个文件系统的限制，所以造一个不落地到任何真实存储的合成文件系统，挂在
 * /dev 下，让设备的存在与否不再依赖挂载在 / 上的是哪个文件系统。
 * 设备集合固定且很小（4 个），挂载时一次性把全部子 dentry 建好、挂进 /dev
 * 根 dentry 的 d_subdirs（dentry_create 传非空 parent 会自动 list_add 进去），
 * 不需要实现 i_op->lookup——vfs_lookup 走内存缓存命中路径就能找到它们。
 * 真正支持运行时注册/注销设备的动态 devfs，留到真有这个需求时再做。 */

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

/* vfs_open() 构造 file_t 时无条件把 f_kind 设成 FILE_KIND_VFS（见 vfs.c）——
 * /dev/null、/dev/zero 的读写都不阻塞，f_kind 留 FILE_KIND_VFS 本可以不出事，
 * 但仍然改成 FILE_KIND_DEVICE：这两个 file 没有真实 inode 背后的数据，语义上
 * 就是设备而不是普通文件，混在 FILE_KIND_VFS 里会让 sys_lseek 之类的壳误以为
 * 它们可以 seek（vfs_lseek 的 SEEK_END 会去解引用 i_size，devfs 的 inode 从
 * 未维护这个字段，结果不可预期）。 */
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

/* 每次 open("/dev") 独立的读游标：next_index 0/1 对应合成的 "."/".."，
 * 2.. 对应 devfs_entries[] 里的真实设备，放在 file->f_private 而不是全局状态——
 * 两个进程各自 opendir("/dev") 必须有独立游标（同一目录被两个进程同时打开）。 */
typedef struct devfs_dir_priv
{
    size_t next_index;
} devfs_dir_priv_t;

/* 与 fatfs_vfs.c 的 fatfs_fill_dirent/fatfs_dirent_reclen 是同一套 struct
 * linux_dirent64 变长记录布局，但不复用那两个 static 函数——它们是围绕 FAT
 * 的 is_dir bool 写的，devfs 需要按 d_type 直接填，硬拉一个共享接口出来
 * 不值得，这里各写各的几行更简单。 */
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
        return NULL; /* sb 故意不回收：devfs 只在启动时挂载一次，失败即 panic 级别的
                      * 配置错误，省下的清理代码换不来实际收益 */
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

void devfs_register(void)
{
    int16_t ret = register_filesystem(&devfs_fs_type);
    if (ret != ENO0_NO_ERROR)
    {
        printf("devfs_register: failed, err=%d\n", ret);
    }
}
