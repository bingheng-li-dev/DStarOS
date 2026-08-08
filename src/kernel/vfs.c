/**
 * vfs.c - 虚拟文件系统（VFS）核心实现
 *
 * 实现 Linux 风格的四层 VFS 抽象：
 *   super_block（超级块）→ inode（索引节点）→ dentry（目录项）→ file（文件对象）
 *
 * 主要功能：
 *   - 文件系统类型注册与注销
 *   - 文件系统挂载与卸载（支持多挂载点）
 *   - 路径解析（vfs_lookup），支持 "."/".."、跨挂载点穿越
 *   - 文件增删改查：vfs_open/close/read/write/lseek
 *   - 目录增删改查：vfs_mkdir/rmdir/rename/unlink
 *   - 进程工作目录：vfs_chdir/vfs_getcwd
 *   - 文件元数据查询：vfs_stat
 */

#include "vfs.h"
#include "kmalloc.h"
#include "errorcode.h"
#include "sync.h"
#include "stringops.h"
#include "proc.h"
#include "cpu.h"

/* ============================================================
 * 全局 VFS 状态
 * ============================================================ */

/* 已挂载的超级块链表（哨兵头）*/
static struct list_head super_block_list;

/* 已挂载的挂载点链表（哨兵头）*/
static struct list_head vfs_mount_list;

/* 已注册的文件系统类型单向链表头 */
static file_system_type_t *file_system_types = NULL;

/* 保护上述三个全局结构的自旋锁 */
osslock_t vfs_fs_lock;

/* VFS 全局根目录项与根挂载点（由 vfs_mount("/", ...) 设置）*/
dentry_t   *vfs_root_dentry = NULL;
vfsmount_t *vfs_root_mount  = NULL;

/* ============================================================
 * 内部工具函数：路径字符串操作
 * ============================================================ */

/**
 * @brief 从路径字符串中提取下一个分量
 * @param[in,out] path    指向路径当前位置的指针（解析后向前推进）
 * @param[out]    name    存放分量名称的缓冲区
 * @param[in]     namelen 缓冲区最大长度（含 '\0'）
 * @retval >0 分量长度
 * @retval  0 路径已结束
 * @retval <0 分量名过长（ENO11_NAME_TOO_LONG）
 * @return 从文件路径里提取目录 / 文件名下一个分量（比如 /a/b/c.txt 会依次拆出 a、b、c.txt）
 */
static int path_next_component(const char **path, char *name, int namelen)
{
    /* 跳过连续的 '/' */
    while (**path == '/')
    {
        (*path)++;
    }

    if (**path == '\0')
    {
        return 0;  /* 路径结束 */
    }

    /* 找到分量结尾 */
    const char *start = *path;
    while (**path != '/' && **path != '\0')
    {
        (*path)++;
    }

    int len = (int)(*path - start);
    if (len >= namelen)
    {
        return ENO11_NAME_TOO_LONG;
    }

    memcpy(name, start, len);
    name[len] = '\0';
    return len;
}

/**
 * @brief 将路径分割为父目录路径和文件名
 * @param[in]  path   输入路径（如 "/dir/file.txt"）
 * @param[out] parent 输出父路径缓冲区（如 "/dir"）
 * @param[in]  psz    parent 缓冲区大小
 * @param[out] name   输出文件名缓冲区（如 "file.txt"）
 * @param[in]  nsz    name 缓冲区大小
 * @note 特殊情况："/file" → parent="/", name="file"；
 *       "file" → parent=".", name="file"；
 *       "/" → parent="/", name=""
 * @retval 0                   成功
 * @retval ENO11_NAME_TOO_LONG 名称过长
 */
static int split_path(const char *path,
                      char *parent, int psz,
                      char *name,   int nsz)
{
    if (!path || !parent || !name)
    {
        return ENO8_NULL_POINTER;
    }

    int plen = (int)strlen(path);

    /* 找最后一个 '/' 的位置 */
    int last_slash = -1;
    for (int i = 0; i < plen; i++)
    {
        if (path[i] == '/')
        {
            last_slash = i;
        }
    }

    if (last_slash < 0)
    {
        /* 没有 '/'：父目录为 "."，文件名为整个路径 */
        if (2 > psz || plen + 1 > nsz)
        {
            return ENO11_NAME_TOO_LONG;
        }
        parent[0] = '.';
        parent[1] = '\0';
        memcpy(name, path, plen + 1);
    }
    else if (last_slash == 0)
    {
        /* 路径形如 "/file"：父目录为 "/" */
        if (2 > psz)
        {
            return ENO11_NAME_TOO_LONG;
        }
        parent[0] = '/';
        parent[1] = '\0';
        int nlen = plen - 1;
        if (nlen + 1 > nsz)
        {
            return ENO11_NAME_TOO_LONG;
        }
        memcpy(name, path + 1, nlen + 1);
    }
    else
    {
        /* 一般情况：分割 */
        if (last_slash + 1 > psz || plen - last_slash > nsz)
        {
            return ENO11_NAME_TOO_LONG;
        }
        memcpy(parent, path, last_slash);
        parent[last_slash] = '\0';
        memcpy(name, path + last_slash + 1, plen - last_slash);
    }

    return 0;
}

/* ============================================================
 * dentry 引用计数
 * ============================================================ */

/**
 * @brief 增加目录项引用计数（内部使用）
 * @param[in] d 目录项指针
 */
static void dentry_get(dentry_t *d)
{
    if (d)
    {
        d->d_ref++;
    }
}

/**
 * @brief 减少目录项引用计数（内部使用）
 * @param[in] d 目录项指针；引用计数归零时调用 d_release 回调并释放内存
 */
static void dentry_put(dentry_t *d)
{
    if (!d)
    {
        return;
    }
    d->d_ref--;
    if (d->d_ref > 0)
    {
        return;
    }

    /* 引用归零：调用释放回调 */
    if (d->d_op && d->d_op->d_release)
    {
        d->d_op->d_release(d);
    }

    /* 从父目录的子列表中摘除 */
    if (d->d_parent != d)
    {
        list_del(&d->d_child);
    }

    kfree(d->d_name);
    kfree(d);
}

/**
 * @brief 引用计数 +1（供 proc.c、fatfs_vfs.c 等外部模块使用）
 * @param[in] d 目录项指针
 */
void dentry_get_pub(dentry_t *d)
{
    dentry_get(d);
}

/**
 * @brief 引用计数 -1，归零时释放（供外部模块使用）
 * @param[in] d 目录项指针
 */
void dentry_put_pub(dentry_t *d)
{
    dentry_put(d);
}

/* ============================================================
 * 文件系统类型注册与注销
 * ============================================================ */

/**
 * @brief 在已注册链表中查找文件系统类型
 * @param[in] name 文件系统名称
 * @param[in] len  名称长度
 * @return 指向对应节点指针域的指针（用于原地插入/删除）
 */
static file_system_type_t **find_filesystem_by_name(const char *name, int len)
{
    if (!name)
    {
        return NULL;
    }

    file_system_type_t **current = &file_system_types;
    while (*current)
    {
        /* 防止前缀匹配：要求名称长度完全相等 */
        if (strncmp((*current)->name, name, len) == 0 && !(*current)->name[len])
        {
            return current;
        }
        current = &(*current)->next;
    }

    /* 返回链表尾部 NULL 指针的地址，供插入新节点使用 */
    return current;
}

/**
 * @brief 向 VFS 注册一种文件系统类型
 * @param[in] fs_type 已填写好的文件系统类型描述符
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO4_BUSY         已注册
 * @retval ENO8_NULL_POINTER 参数非法
 */
int16_t register_filesystem(file_system_type_t *fs_type)
{
    file_system_type_t **fs_type_ptr;
    int16_t ret = ENO0_NO_ERROR;

    if (!fs_type || !fs_type->name || !fs_type->mount)
    {
        return ENO8_NULL_POINTER;
    }

    /* next != NULL 说明该节点已挂入链表（已注册）*/
    if (fs_type->next)
    {
        return ENO4_BUSY;
    }

    spinlock_acquire(&vfs_fs_lock);

    int nlen = (int)strlen(fs_type->name);
    fs_type_ptr = find_filesystem_by_name(fs_type->name, nlen);
    if (*fs_type_ptr)
    {
        ret = ENO4_BUSY;
    }
    else
    {
        /* 插入到链表尾部 */
        *fs_type_ptr = fs_type;
    }

    spinlock_release(&vfs_fs_lock);

    return ret;
}

/**
 * @brief 从 VFS 注销一种文件系统类型
 * @param[in] fs_type 要注销的文件系统类型描述符
 * @retval ENO0_NO_ERROR    成功
 * @retval ENO5_NOSUCH_ENTRY 未找到
 */
int16_t unregister_filesystem(file_system_type_t *fs_type)
{
    file_system_type_t **fs_type_ptr = &file_system_types;

    if (!fs_type)
    {
        return ENO8_NULL_POINTER;
    }

    spinlock_acquire(&vfs_fs_lock);

    while (*fs_type_ptr)
    {
        if (*fs_type_ptr == fs_type)
        {
            *fs_type_ptr = fs_type->next;
            fs_type->next = NULL;
            spinlock_release(&vfs_fs_lock);
            return ENO0_NO_ERROR;
        }
        fs_type_ptr = &(*fs_type_ptr)->next;
    }

    spinlock_release(&vfs_fs_lock);

    return ENO5_NOSUCH_ENTRY;
}

/**
 * @brief 按名称获取已注册的文件系统类型
 * @param[in] name 文件系统名称字符串
 * @return 匹配的 file_system_type_t 指针；未找到返回 NULL
 */
static file_system_type_t *get_fs_type_by_name(const char *name)
{
    if (!name)
    {
        return NULL;
    }

    int nlen = (int)strlen(name);
    spinlock_acquire(&vfs_fs_lock);
    file_system_type_t *fs = *find_filesystem_by_name(name, nlen);
    spinlock_release(&vfs_fs_lock);

    return fs;
}

/* ============================================================
 * 超级块操作
 * ============================================================ */

/**
 * @brief 分配并初始化超级块，加入全局链表
 * @param[in] fs_id        文件系统标识符字符串
 * @param[in] block_size   块大小（字节）
 * @param[in] total_blocks 总块数
 * @param[in] s_op         超级块操作函数集
 * @param[in] private_data 底层文件系统私有数据指针
 * @return 新超级块指针；内存不足返回 NULL
 */
super_block_t *alloc_super_block(
    const char *fs_id,
    uint64_t block_size,
    uint64_t total_blocks,
    super_block_operations_t *s_op,
    void *private_data)
{
    super_block_t *sb = (super_block_t *)kmalloc(sizeof(super_block_t));
    if (!sb)
    {
        return NULL;
    }

    sb->s_fs_id       = (char *)fs_id;
    sb->s_block_size  = block_size;
    sb->s_total_blocks = total_blocks;
    sb->s_type        = NULL;
    sb->s_op          = s_op;
    sb->s_root_inode  = NULL;
    sb->s_private     = private_data;
    sb->s_mount       = NULL;

    INIT_LIST_HEAD(&sb->s_list_linker);
    list_add(&sb->s_list_linker, &super_block_list);

    return sb;
}

/**
 * @brief 从全局链表摘除并释放超级块
 * @param[in] sb 要销毁的超级块指针
 * @note 调用前应先完成文件系统的卸载（unmount）。
 */
void destroy_super_block(super_block_t *sb)
{
    if (!sb)
    {
        return;
    }
    list_del(&sb->s_list_linker);
    kfree(sb);
}

/* ============================================================
 * inode 操作
 * ============================================================ */

/**
 * @brief 通过超级块的 alloc_inode 回调分配 inode
 * @param[in] sb 所属超级块
 * @return 新 inode 指针；失败返回 NULL
 */
inode_t *alloc_inode(super_block_t *sb)
{
    if (!sb || !sb->s_op || !sb->s_op->alloc_inode)
    {
        return NULL;
    }
    return sb->s_op->alloc_inode(sb);
}

/**
 * @brief 通过超级块的 destory_inode 回调释放 inode
 * @param[in] inode 要释放的 inode 指针
 */
void destory_inode(inode_t *inode)
{
    if (!inode || !inode->i_sb || !inode->i_sb->s_op ||
        !inode->i_sb->s_op->destory_inode)
    {
        return;
    }
    inode->i_sb->s_op->destory_inode(inode);
}

/* ============================================================
 * 目录项操作
 * ============================================================ */

/**
 * @brief 分配并初始化一个目录项
 * @param[in] name   目录项名称（内部复制，调用者可释放）
 * @param[in] inode  对应 inode（NULL 表示负目录项）
 * @param[in] parent 父目录项（NULL 表示文件系统局部根，d_parent 指向自身）
 * @param[in] op     目录项操作集（可为 NULL）
 * @return 新目录项指针；内存不足返回 NULL
 */
dentry_t *dentry_create(const char *name, inode_t *inode,
                        dentry_t *parent, dentry_operations_t *op)
{
    if (!name)
    {
        return NULL;
    }

    dentry_t *d = (dentry_t *)kmalloc(sizeof(dentry_t));
    if (!d)
    {
        return NULL;
    }

    /* 复制名称字符串（生命周期独立于调用者），调用者可以随便释放自己的字符串 */
    int len = (int)strlen(name);
    d->d_name = (char *)kmalloc(len + 1);
    if (!d->d_name)
    {
        kfree(d);
        return NULL;
    }
    memcpy(d->d_name, name, len + 1);

    d->d_inode   = inode;
    d->d_op      = op;
    d->d_ref     = 1;
    d->d_mounted = NULL;

    /* 初始化子目录链表头（空子目录列表）*/
    INIT_LIST_HEAD(&d->d_subdirs);

    if (!parent) /* 没有父目录 → 这是文件系统根目录 */
    {
        /* 文件系统局部根：父指向自身，d_child 为孤立节点，不挂到任何父链表 */
        d->d_parent = d;
        INIT_LIST_HEAD(&d->d_child);
    }
    else
    {
        d->d_parent = parent;
        list_add(&d->d_child, &parent->d_subdirs);
    }

    return d;
}

/**
 * @brief 在父目录的内存缓存（d_subdirs 链表）中按名称查找子目录项
 * @param[in] parent 父目录项
 * @param[in] name   要查找的名称
 * @return 找到时返回引用计数 +1 的子目录项；未找到返回 NULL
 */
dentry_t *dentry_lookup(dentry_t *parent, const char *name)
{
    if (!parent || !name)
    {
        return NULL;
    }

    struct list_head *pos;
    dentry_t *child;

    list_for_each(pos, &parent->d_subdirs)
    {
        child = list_entry(pos, dentry_t, d_child);
        if (strncmp(child->d_name, name, VFS_NAME_MAX) == 0)
        {
            dentry_get(child);
            return child;
        }
    }
    return NULL;
}

/* ============================================================
 * 辅助：获取挂载在指定 super_block 上的 vfsmount
 * ============================================================ */
static vfsmount_t *find_mount_by_sb(super_block_t *sb)
{
    struct list_head *pos;
    vfsmount_t *mnt;

    list_for_each(pos, &vfs_mount_list)
    {
        mnt = list_entry(pos, vfsmount_t, mnt_list_linker);
        if (mnt->mnt_sb == sb)
        {
            return mnt;
        }
    }
    return NULL;
}

/* ============================================================
 * VFS 初始化
 * ============================================================ */

/**
 * @brief 初始化 VFS 全局数据结构
 * @note 必须在 fs_init() 中最先调用。
 */
void vfs_init(void)
{
    INIT_LIST_HEAD(&super_block_list);
    INIT_LIST_HEAD(&vfs_mount_list);
    spinlock_init(&vfs_fs_lock);
    file_system_types = NULL;
    vfs_root_dentry   = NULL;
    vfs_root_mount    = NULL;
}

/* ============================================================
 * 核心路径解析：vfs_lookup
 * ============================================================ */

/**
 * @brief 将路径字符串解析为对应的目录项
 * @param[in] path 要解析的路径字符串（绝对或相对）
 * @return 持有一个引用计数的目录项；路径不存在或出错返回 NULL
 * @note 调用者负责调用 dentry_put_pub() 释放返回的引用。
 * @details 解析算法：
 *  -# 若路径以 '/' 开头，起始节点为 vfs_root_dentry；否则为 proc_cwd（NULL 时回退到根）。
 *  -# 逐分量循环：
 *     - 进入节点前检查 d_mounted，穿越到被挂载文件系统的根；
 *     - "." 保持不动；".." 向上，若已是文件系统局部根则反向穿越挂载点；
 *     - 普通分量先查内存缓存（dentry_lookup），未命中则调 i_op->lookup。
 */
dentry_t *vfs_lookup(const char *path)
{
    if (!path)
    {
        return NULL;
    }
    if (!vfs_root_dentry)
    {
        return NULL;  /* 根文件系统尚未挂载 */
    }

    dentry_t *cur;

    if (path[0] == '/')
    {
        /* 绝对路径：从全局根出发 */
        cur = vfs_root_dentry;
        dentry_get(cur);
        /* 跳过开头的所有 '/' */
        while (*path == '/')
        {
            path++;
        }
    }
    else
    {
        /* 相对路径：从当前进程的工作目录出发 */
        pcb_t *cur_proc = getCurrentProc();
        if (cur_proc && cur_proc->proc_cwd)
        {
            cur = cur_proc->proc_cwd;
        }
        else
        {
            cur = vfs_root_dentry;
        }
        dentry_get(cur);
    }

    /* 纯 "/" 路径或空相对路径：直接返回当前节点 */
    if (*path == '\0') // while (*path == '/') path++;
    {
        return cur;
    }

    char comp[VFS_NAME_MAX];

    while (*path)
    {
        int len = path_next_component(&path, comp, VFS_NAME_MAX);
        if (len == 0)
        {
            break;
        }
        if (len < 0)
        {
            /* 分量名过长 */
            dentry_put(cur);
            return NULL;
        }

        /* 穿越挂载点：若当前节点被某文件系统挂载，则进入被挂载文件系统的根 */
        while (cur->d_mounted)
        {
            vfsmount_t *mnt = cur->d_mounted;
            if (!mnt->mnt_sb || !mnt->mnt_sb->s_root_inode ||
                !mnt->mnt_sb->s_root_inode->i_dentry)
            {
                break;
            }
            dentry_t *mnt_root = mnt->mnt_sb->s_root_inode->i_dentry;
            dentry_get(mnt_root);
            dentry_put(cur);
            cur = mnt_root;
        }

        /* 处理 "." 分量（保持不动）*/
        if (comp[0] == '.' && comp[1] == '\0')
        {
            continue;
        }

        /* 处理 ".." 分量（向上一级）*/
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0')
        {
            /* 已在全局根，".." 原地不动 */
            if (cur == vfs_root_dentry)
            {
                continue;
            }

            /* 若当前节点是文件系统局部根（d_parent 指向自身），需反向穿越挂载点 */
            if (cur->d_parent == cur)
            {
                /* 在挂载点链表中找到对应的 vfsmount，取宿主目录项的父节点 */
                vfsmount_t *mnt = NULL;
                spinlock_acquire(&vfs_fs_lock);
                if (cur->d_inode && cur->d_inode->i_sb)
                {
                    mnt = find_mount_by_sb(cur->d_inode->i_sb);
                }
                spinlock_release(&vfs_fs_lock);

                if (mnt && mnt->mnt_host_dentry && mnt->mnt_host_dentry->d_parent)
                {
                    dentry_t *host_parent = mnt->mnt_host_dentry->d_parent;
                    dentry_get(host_parent);
                    dentry_put(cur);
                    cur = host_parent;
                }
                /* 如果找不到宿主，保持在当前局部根（理论上不应发生）*/
                continue;
            }

            /* 普通 ".." 向上一级 */
            dentry_t *parent = cur->d_parent;
            dentry_get(parent);
            dentry_put(cur);
            cur = parent;
            continue;
        }

        /* 确认当前节点是目录 */
        if (!cur->d_inode || !S_ISDIR(cur->d_inode->i_mode))
        {
            dentry_put(cur);
            return NULL;
        }

        /* 先查内存缓存（dentry_lookup 已包含 dentry_get）*/
        dentry_t *next = dentry_lookup(cur, comp);

        /* 缓存未命中：委托底层文件系统查找 */
        if (!next)
        {
            if (!cur->d_inode->i_op || !cur->d_inode->i_op->lookup)
            {
                dentry_put(cur);
                return NULL;
            }
            next = cur->d_inode->i_op->lookup(cur->d_inode, comp);
        }

        if (!next) /* 底层文件系统也未找到 */
        {
            dentry_put(cur);
            return NULL;
        }

        dentry_put(cur);
        cur = next;
    }

    return cur;
}

/* ============================================================
 * 挂载与卸载
 * ============================================================ */

/**
 * @brief 挂载文件系统
 * @param[in] path    挂载点路径（"/" 表示根文件系统）
 * @param[in] fs_type 文件系统类型名称（如 "fatfs"）
 * @param[in] data    传递给具体文件系统的额外参数（可为 NULL）
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO8_NULL_POINTER 参数为 NULL
 * @retval ENO5_NOSUCH_ENTRY 文件系统类型未注册或挂载点不存在
 * @retval ENO13_NO_FS       文件系统挂载回调失败
 * @retval ENO1_NOMORE_MEM   内存不足
 * @retval ENO4_BUSY         挂载点已被占用
 */
int vfs_mount(const char *path, const char *fs_type, void *data)
{
    if (!path || !fs_type)
    {
        return ENO8_NULL_POINTER;
    }

    /* 1. 查找文件系统类型 */
    file_system_type_t *fst = get_fs_type_by_name(fs_type);
    if (!fst)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    /* 2. 调用文件系统挂载回调，获取根目录项 */
    dentry_t *root_dentry = fst->mount(fst, path, data); // mount后会生成一个新的超级块、inode和根目录项
    if (!root_dentry)
    {
        return ENO13_NO_FS;
    }

    /* 3. 分配 vfsmount_t */
    vfsmount_t *mnt = (vfsmount_t *)kmalloc(sizeof(vfsmount_t));
    if (!mnt)
    {
        return ENO1_NOMORE_MEM;
    }

    /* 复制挂载点路径字符串 */
    int plen = (int)strlen(path);
    mnt->mnt_path = (char *)kmalloc(plen + 1);
    if (!mnt->mnt_path)
    {
        kfree(mnt);
        return ENO1_NOMORE_MEM;
    }
    memcpy(mnt->mnt_path, path, plen + 1);

    /* 挂载根文件系统时，在某些实现中，sb尚未初始化，此时为唯一合法mnt->mnt_sb=NULL。Fatfs出现该情况非法 */
    mnt->mnt_sb = root_dentry->d_inode ? root_dentry->d_inode->i_sb : NULL;
    mnt->mnt_host_dentry = NULL;
    INIT_LIST_HEAD(&mnt->mnt_list_linker);

    /* 4. 挂载点处理 */
    if (path[0] == '/' && path[1] == '\0')
    {
        /* 挂载为根文件系统 */
        vfs_root_dentry = root_dentry;
        vfs_root_mount  = mnt;
        mnt->mnt_host_dentry = NULL;
    }
    else
    {
        /* 挂载到已有路径上 */
        dentry_t *host = vfs_lookup(path);
        if (!host)
        {
            kfree(mnt->mnt_path);
            kfree(mnt);
            return ENO5_NOSUCH_ENTRY;
        }
        if (!host->d_inode || !S_ISDIR(host->d_inode->i_mode))
        {
            dentry_put(host);
            kfree(mnt->mnt_path);
            kfree(mnt);
            return ENO9_NOT_DIR;
        }
        if (host->d_mounted) /* 当前目录项已经挂载了其他文件系统 */
        {
            dentry_put(host);
            kfree(mnt->mnt_path);
            kfree(mnt);
            return ENO4_BUSY;
        }
        host->d_mounted      = mnt;
        mnt->mnt_host_dentry = host;
        /* 持有宿主 dentry 的引用（挂载期间不允许删除）*/
    }

    /* 同步挂载信息到超级块 */
    if (mnt->mnt_sb)
    {
        mnt->mnt_sb->s_mount = mnt;
    }

    spinlock_acquire(&vfs_fs_lock);
    list_add(&mnt->mnt_list_linker, &vfs_mount_list);
    spinlock_release(&vfs_fs_lock);

    return ENO0_NO_ERROR;
}

/**
 * @brief 卸载文件系统
 * @param[in] path 挂载点路径
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO8_NULL_POINTER 参数为 NULL
 * @retval ENO16_PERM        尝试卸载根文件系统（"/"）
 * @retval ENO5_NOSUCH_ENTRY 挂载点未找到
 */
int vfs_unmount(const char *path)
{
    if (!path)
    {
        return ENO8_NULL_POINTER;
    }

    /* 不允许卸载根文件系统 */
    if (path[0] == '/' && path[1] == '\0')
    {
        return ENO16_PERM;
    }

    /* 在挂载点链表中查找 */
    vfsmount_t *target = NULL;
    spinlock_acquire(&vfs_fs_lock);
    struct list_head *pos;
    list_for_each(pos, &vfs_mount_list)
    {
        vfsmount_t *m = list_entry(pos, vfsmount_t, mnt_list_linker);
        if (strncmp(m->mnt_path, path, VFS_PATH_MAX) == 0)
        {
            target = m;
            break;
        }
    }
    spinlock_release(&vfs_fs_lock);

    if (!target)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    /* 同步并卸载底层文件系统 */
    if (target->mnt_sb && target->mnt_sb->s_op)
    {
        if (target->mnt_sb->s_op->sync_fs)
        {
            target->mnt_sb->s_op->sync_fs(target->mnt_sb);
        }
        if (target->mnt_sb->s_op->unmount)
        {
            target->mnt_sb->s_op->unmount(target->mnt_sb);
        }
    }

    /* 清除宿主目录项上的挂载标记 */
    if (target->mnt_host_dentry)
    {
        target->mnt_host_dentry->d_mounted = NULL;
        dentry_put(target->mnt_host_dentry);
    }

    /* 销毁超级块 */
    if (target->mnt_sb)
    {
        destroy_super_block(target->mnt_sb);
    }

    /* 从链表中摘除并释放 vfsmount */
    spinlock_acquire(&vfs_fs_lock);
    list_del(&target->mnt_list_linker);
    spinlock_release(&vfs_fs_lock);

    kfree(target->mnt_path);
    kfree(target);

    return ENO0_NO_ERROR;
}

/* ============================================================
 * 文件操作
 * ============================================================ */

/**
 * @brief 打开（或创建）文件
 * @param[in] path 文件路径
 * @param[in] mode 打开模式标志（O_RDONLY/O_WRONLY/O_RDWR/O_CREAT 等）
 * @return 成功返回打开的 file_t 指针；失败返回 NULL
 * @note 流程：路径解析 → O_CREAT 时创建文件 → 分配 file_t → 调用底层 open 回调
 */
file_t *vfs_open(const char *path, int mode)
{
    if (!path)
    {
        return NULL;
    }
    if (!vfs_root_dentry)
    {
        return NULL;
    }

    dentry_t *target = vfs_lookup(path);

    if (!target)
    {
        /* 文件不存在 */
        if (!(mode & O_CREAT))
        {
            return NULL;
        }

        /* 分离父目录路径和文件名 */
        char ppath[VFS_PATH_MAX], fname[VFS_NAME_MAX];
        if (split_path(path, ppath, VFS_PATH_MAX, fname, VFS_NAME_MAX) < 0)
        {
            return NULL;
        }

        /* 查找父目录 */
        dentry_t *parent = vfs_lookup(ppath);
        if (!parent)
        {
            return NULL;
        }
        if (!parent->d_inode || !S_ISDIR(parent->d_inode->i_mode))
        {
            dentry_put(parent);
            return NULL;
        }

        /* 创建负目录项（d_inode = NULL），然后调底层 create */
        dentry_t *new_d = dentry_create(fname, NULL, parent, NULL);
        if (!new_d)
        {
            dentry_put(parent);
            return NULL;
        }

        if (!parent->d_inode->i_op || !parent->d_inode->i_op->create)
        {
            list_del(&new_d->d_child);
            kfree(new_d->d_name);
            kfree(new_d);
            dentry_put(parent);
            return NULL;
        }

        int ret = parent->d_inode->i_op->create(
                      parent->d_inode, new_d, S_IFREG | 0644);
        dentry_put(parent);
        if (ret != ENO0_NO_ERROR)
        {
            list_del(&new_d->d_child);
            kfree(new_d->d_name);
            kfree(new_d);
            return NULL;
        }

        target = new_d;
    }
    else
    {
        /* 文件已存在 */
        if ((mode & O_CREAT) && (mode & O_EXCL))
        {
            /* O_CREAT | O_EXCL：文件已存在则失败 */
            dentry_put(target);
            return NULL;
        }
    }

    /* target 现在指向有效的 dentry，且持有一个引用计数 */
    if (!target->d_inode)
    {
        dentry_put(target);
        return NULL;
    }

    /* 不允许对目录使用写模式打开 */
    if (S_ISDIR(target->d_inode->i_mode))
    {
        int acc = mode & O_ACCMODE;
        if (acc == O_WRONLY || acc == O_RDWR)
        {
            dentry_put(target);
            return NULL;
        }
    }

    /* 分配 file_t */
    file_t *file = (file_t *)kmalloc(sizeof(file_t));
    if (!file)
    {
        dentry_put(target);
        return NULL;
    }

    /* 复制路径字符串（调试用）*/
    int pathlen = (int)strlen(path);
    file->f_path = (char *)kmalloc(pathlen + 1);
    if (!file->f_path)
    {
        kfree(file);
        dentry_put(target);
        return NULL;
    }
    memcpy(file->f_path, path, pathlen + 1);

    file->f_inode   = target->d_inode;
    file->f_dentry  = target;          /* 持有引用，防止 dentry 被释放 */
    file->f_op      = target->d_inode->i_fop;  /* 从 inode 获取操作集 */
    file->f_mode    = mode;
    file->f_count   = 1;
    file->f_private = NULL;
    file->f_vfsmount = NULL;

    /* O_APPEND 模式：初始位置设为文件末尾 */
    if (mode & O_APPEND)
    {
        file->f_pos = (off_t)target->d_inode->i_size;
    }
    else
    {
        file->f_pos = 0;
    }

    /* 调用底层 open 回调（分配底层资源，如 FIL*）*/
    if (file->f_op && file->f_op->open)
    {
        int ret = file->f_op->open(target->d_inode, file, mode);
        if (ret != ENO0_NO_ERROR)
        {
            kfree(file->f_path);
            kfree(file);
            dentry_put(target);
            return NULL;
        }
    }

    /* O_TRUNC：打开成功后截断文件（底层 open 处理，此处更新 inode 大小）*/
    if ((mode & O_TRUNC) && !S_ISDIR(target->d_inode->i_mode))
    {
        file->f_inode->i_size = 0;
        file->f_pos = 0;
    }

    return file;
}

/**
 * @brief 关闭文件
 * @param[in] file 要关闭的文件对象
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO8_NULL_POINTER 参数为 NULL
 * @note 减少引用计数，归零时调用底层 close 回调并释放所有资源。
 */
int vfs_close(file_t *file)
{
    if (!file)
    {
        return ENO8_NULL_POINTER;
    }

    /* 引用计数减 1 */
    file->f_count--;
    if (file->f_count > 0)
    {
        return ENO0_NO_ERROR;
    }

    /* 调用底层关闭回调（释放底层资源，如 FIL*）*/
    if (file->f_op && file->f_op->close)
    {
        file->f_op->close(file);
    }

    /* 释放持有的目录项引用 */
    if (file->f_dentry)
    {
        dentry_put(file->f_dentry);
    }

    kfree(file->f_path);
    kfree(file);
    return ENO0_NO_ERROR;
}

/**
 * @brief 从文件中读取数据
 * @param[in]  file 文件对象
 * @param[out] buf  接收数据的缓冲区
 * @param[in]  len  请求读取的字节数
 * @return 实际读取字节数；负值表示错误码
 */
ssize_t vfs_read(file_t *file, void *buf, size_t len)
{
    if (!file || !buf)
    {
        return ENO8_NULL_POINTER;
    }

    /* 检查访问权限：只写文件不允许读 */
    if ((file->f_mode & O_ACCMODE) == O_WRONLY)
    {
        return ENO16_PERM;
    }

    if (!file->f_op || !file->f_op->read)
    {
        return ENO8_NULL_POINTER;
    }

    return file->f_op->read(file, buf, len);
}

/**
 * @brief 向文件中写入数据
 * @param[in] file 文件对象
 * @param[in] buf  待写入数据缓冲区
 * @param[in] len  写入字节数
 * @return 实际写入字节数；负值表示错误码
 */
ssize_t vfs_write(file_t *file, const void *buf, size_t len)
{
    if (!file || !buf)
    {
        return ENO8_NULL_POINTER;
    }

    /* 检查访问权限：只读文件不允许写 */
    if ((file->f_mode & O_ACCMODE) == O_RDONLY)
    {
        return ENO16_PERM;
    }

    if (!file->f_op || !file->f_op->write)
    {
        return ENO8_NULL_POINTER;
    }

    /* O_APPEND 模式：每次写操作前将位置移到文件末尾 */
    if (file->f_mode & O_APPEND)
    {
        file->f_pos = (off_t)file->f_inode->i_size;
    }

    return file->f_op->write(file, buf, len);
}

/**
 * @brief 截断或扩展文件到指定大小
 * @param[in] path 目标文件路径
 * @param[in] size 目标大小（字节）；0 即清空；大于当前大小时扩展
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO8_NULL_POINTER 参数为 NULL 或文件系统无 truncate 支持
 * @retval ENO13_NO_FS       文件系统未挂载
 * @retval ENO5_NOSUCH_ENTRY 路径不存在
 * @retval ENO10_IS_DIR      目标是目录
 */
int vfs_truncate(const char *path, uint64_t size)
{
    if (!path)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    dentry_t *d = vfs_lookup(path);
    if (!d)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    if (!d->d_inode)
    {
        dentry_put(d);
        return ENO5_NOSUCH_ENTRY;
    }

    /* 目录不允许截断 */
    if (S_ISDIR(d->d_inode->i_mode))
    {
        dentry_put(d);
        return ENO10_IS_DIR;
    }

    if (!d->d_inode->i_op || !d->d_inode->i_op->truncate)
    {
        dentry_put(d);
        return ENO8_NULL_POINTER;
    }

    int ret = d->d_inode->i_op->truncate(d->d_inode, size);
    dentry_put(d);
    return ret;
}

/**
 * @brief 移动文件读写位置
 * @param[in] file   文件对象
 * @param[in] offset 偏移量
 * @param[in] whence 参照点（SEEK_SET / SEEK_CUR / SEEK_END）
 * @return 新的文件位置；负值表示错误码
 */
off_t vfs_lseek(file_t *file, off_t offset, int whence)
{
    if (!file)
    {
        return (off_t)ENO8_NULL_POINTER;
    }

    off_t new_pos;

    if (whence == SEEK_SET)
    {
        new_pos = offset;
    }
    else if (whence == SEEK_CUR)
    {
        new_pos = file->f_pos + offset;
    }
    else if (whence == SEEK_END)
    {
        new_pos = (off_t)file->f_inode->i_size + offset;
    }
    else
    {
        return (off_t)ENO6_INVAL_PARAM;
    }

    if (new_pos < 0)
    {
        return (off_t)ENO6_INVAL_PARAM;
    }

    /* 若底层文件系统提供了 lseek 回调，委托给它处理 */
    if (file->f_op && file->f_op->lseek)
    {
        return file->f_op->lseek(file, offset, whence);
    }

    /* 默认 VFS 层实现：直接更新 f_pos */
    file->f_pos = new_pos;
    return new_pos;
}

/* ============================================================
 * 目录操作
 * ============================================================ */

/**
 * @brief 创建目录
 * @param[in] path 要创建的目录路径
 * @param[in] mode 权限位（如 0755）
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO7_EXISTS       目录已存在
 * @retval ENO5_NOSUCH_ENTRY 父目录不存在
 * @retval ENO9_NOT_DIR      父路径不是目录
 */
int vfs_mkdir(const char *path, mode_t mode)
{
    if (!path)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    /* 检查目录是否已存在 */
    dentry_t *ex = vfs_lookup(path);
    if (ex)
    {
        dentry_put(ex);
        return ENO7_EXISTS;
    }

    /* 分离父路径和目录名 */
    char ppath[VFS_PATH_MAX], dname[VFS_NAME_MAX];
    if (split_path(path, ppath, VFS_PATH_MAX, dname, VFS_NAME_MAX) < 0)
    {
        return ENO11_NAME_TOO_LONG;
    }

    /* 查找父目录 */
    dentry_t *parent = vfs_lookup(ppath);
    if (!parent)
    {
        return ENO5_NOSUCH_ENTRY;
    }
    if (!parent->d_inode || !S_ISDIR(parent->d_inode->i_mode))
    {
        dentry_put(parent);
        return ENO9_NOT_DIR;
    }

    /* 创建负目录项，然后调底层 mkdir */
    dentry_t *nd = dentry_create(dname, NULL, parent, NULL);
    if (!nd)
    {
        dentry_put(parent);
        return ENO1_NOMORE_MEM;
    }

    if (!parent->d_inode->i_op || !parent->d_inode->i_op->mkdir)
    {
        list_del(&nd->d_child);
        kfree(nd->d_name);
        kfree(nd);
        dentry_put(parent);
        return ENO8_NULL_POINTER;
    }

    int ret = parent->d_inode->i_op->mkdir(parent->d_inode, nd,
                                            S_IFDIR | (mode & 0777));
    dentry_put(parent);
    if (ret != ENO0_NO_ERROR)
    {
        /* 创建失败：清理负目录项 */
        list_del(&nd->d_child);
        kfree(nd->d_name);
        kfree(nd);
    }
    return ret;
}

/**
 * @brief 删除空目录
 * @param[in] path 要删除的目录路径
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO5_NOSUCH_ENTRY 目录不存在
 * @retval ENO9_NOT_DIR      目标不是目录
 * @retval ENO12_NOT_EMPTY   目录非空
 * @retval ENO4_BUSY         目录是挂载点
 */
int vfs_rmdir(const char *path)
{
    if (!path)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    dentry_t *d = vfs_lookup(path);
    if (!d)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    /* 必须是目录 */
    if (!d->d_inode || !S_ISDIR(d->d_inode->i_mode))
    {
        dentry_put(d);
        return ENO9_NOT_DIR;
    }

    /* 目录非空：不允许删除 */
    if (!list_empty(&d->d_subdirs))
    {
        dentry_put(d);
        return ENO12_NOT_EMPTY;
    }

    /* 目录是挂载点：忙 */
    if (d->d_mounted)
    {
        dentry_put(d);
        return ENO4_BUSY;
    }

    dentry_t *parent = d->d_parent;
    if (!parent || !parent->d_inode ||
        !parent->d_inode->i_op || !parent->d_inode->i_op->rmdir)
    {
        dentry_put(d);
        return ENO8_NULL_POINTER;
    }

    int was_cached = (d->d_ref > 1);
    int ret = parent->d_inode->i_op->rmdir(parent->d_inode, d);
    if (ret == ENO0_NO_ERROR)
    {
        /* Evict from dentry cache and destroy the inode */
        list_del_init(&d->d_child);
        if (d->d_inode)
        {
            destory_inode(d->d_inode);
            d->d_inode = NULL;
        }
    }
    dentry_put(d);  /* drop the lookup ref from vfs_lookup */
    if (ret == ENO0_NO_ERROR && was_cached)
    {
        dentry_put(d);  /* drop the cache ownership ref left by vfs_mkdir */
    }
    return ret;
}

/**
 * @brief 删除普通文件
 * @param[in] path 要删除的文件路径
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO5_NOSUCH_ENTRY 文件不存在
 * @retval ENO10_IS_DIR      目标是目录（应使用 vfs_rmdir）
 */
int vfs_unlink(const char *path)
{
    if (!path)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    dentry_t *d = vfs_lookup(path);
    if (!d)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    /* 不允许删除目录（应使用 rmdir）*/
    if (d->d_inode && S_ISDIR(d->d_inode->i_mode))
    {
        dentry_put(d);
        return ENO10_IS_DIR;
    }

    dentry_t *parent = d->d_parent;
    if (!parent || !parent->d_inode ||
        !parent->d_inode->i_op || !parent->d_inode->i_op->unlink)
    {
        dentry_put(d);
        return ENO8_NULL_POINTER;
    }

    int was_cached = (d->d_ref > 1);
    int ret = parent->d_inode->i_op->unlink(parent->d_inode, d);
    if (ret == ENO0_NO_ERROR)
    {
        /* Evict from dentry cache and destroy the inode */
        list_del_init(&d->d_child);
        if (d->d_inode)
        {
            destory_inode(d->d_inode);
            d->d_inode = NULL;
        }
    }
    dentry_put(d);  /* drop the lookup ref from vfs_lookup */
    if (ret == ENO0_NO_ERROR && was_cached)
    {
        dentry_put(d);  /* drop the cache ownership ref (e.g. from vfs_open still in cache) */
    }
    return ret;
}

/**
 * @brief 重命名或移动文件/目录
 * @param[in] oldpath 源路径
 * @param[in] newpath 目标路径
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO5_NOSUCH_ENTRY 源路径不存在
 * @retval ENO14_CROSS_DEV   跨挂载点重命名（不支持）
 */
int vfs_rename(const char *oldpath, const char *newpath)
{
    if (!oldpath || !newpath)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    dentry_t *old_d = vfs_lookup(oldpath);
    if (!old_d)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    /* 分离新路径的父目录和文件名 */
    char new_ppath[VFS_PATH_MAX], new_name[VFS_NAME_MAX];
    if (split_path(newpath, new_ppath, VFS_PATH_MAX, new_name, VFS_NAME_MAX) < 0)
    {
        dentry_put(old_d);
        return ENO11_NAME_TOO_LONG;
    }

    dentry_t *new_parent = vfs_lookup(new_ppath);
    if (!new_parent)
    {
        dentry_put(old_d);
        return ENO5_NOSUCH_ENTRY;
    }
    if (!new_parent->d_inode || !S_ISDIR(new_parent->d_inode->i_mode))
    {
        dentry_put(old_d);
        dentry_put(new_parent);
        return ENO9_NOT_DIR;
    }

    /* 不允许跨挂载点重命名 */
    if (old_d->d_inode && new_parent->d_inode &&
        old_d->d_inode->i_sb != new_parent->d_inode->i_sb)
    {
        dentry_put(old_d);
        dentry_put(new_parent);
        return ENO14_CROSS_DEV;
    }

    /* 创建用于传递给底层的新目录项（负目录项）*/
    dentry_t *new_d = dentry_create(new_name, NULL, new_parent, NULL);
    if (!new_d)
    {
        dentry_put(old_d);
        dentry_put(new_parent);
        return ENO1_NOMORE_MEM;
    }

    /* 调用底层 rename 回调 */
    int ret = ENO0_NO_ERROR;
    if (old_d->d_parent && old_d->d_parent->d_inode &&
        old_d->d_parent->d_inode->i_op &&
        old_d->d_parent->d_inode->i_op->rename)
    {
        ret = old_d->d_parent->d_inode->i_op->rename(
                  old_d->d_parent->d_inode, old_d,
                  new_parent->d_inode, new_d);
    }

    if (ret != ENO0_NO_ERROR)
    {
        /* 失败：清理临时新目录项 */
        list_del(&new_d->d_child);
        kfree(new_d->d_name);
        kfree(new_d);
        dentry_put(old_d);
        dentry_put(new_parent);
        return ret;
    }

    /* 成功：将原目录项移到新父目录下，更新名称 */
    list_del(&old_d->d_child);
    kfree(old_d->d_name);
    int nl = (int)strlen(new_name);
    old_d->d_name = (char *)kmalloc(nl + 1);
    if (old_d->d_name)
    {
        memcpy(old_d->d_name, new_name, nl + 1);
    }
    old_d->d_parent = new_parent;
    list_add(&old_d->d_child, &new_parent->d_subdirs);

    /* 释放用于传递的临时新目录项 */
    list_del(&new_d->d_child);
    kfree(new_d->d_name);
    kfree(new_d);

    dentry_put(old_d);
    dentry_put(new_parent);
    return ENO0_NO_ERROR;
}

/**
 * @brief 创建硬链接（FAT 文件系统不支持，始终返回 ENO16_PERM）
 * @param[in] oldpath 源文件路径
 * @param[in] newpath 链接路径
 * @retval ENO16_PERM 始终返回（FAT 不支持硬链接）
 */
int vfs_link(const char *oldpath, const char *newpath)
{
    (void)oldpath;
    (void)newpath;
    return ENO16_PERM;  /* FAT 文件系统不支持硬链接 */
}

/**
 * @brief 创建符号链接（FAT 文件系统不支持，始终返回 ENO16_PERM）
 * @param[in] target   目标路径
 * @param[in] linkpath 链接路径
 * @retval ENO16_PERM 始终返回（FAT 不支持符号链接）
 */
int vfs_symlink(const char *target, const char *linkpath)
{
    (void)target;
    (void)linkpath;
    return ENO16_PERM;  /* FAT 文件系统不支持符号链接 */
}

/* ============================================================
 * 进程工作目录
 * ============================================================ */

/**
 * @brief 改变当前进程的工作目录
 * @param[in] path 目标目录路径
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO8_NULL_POINTER 参数为 NULL
 * @retval ENO5_NOSUCH_ENTRY 路径不存在
 * @retval ENO9_NOT_DIR      路径不是目录
 */
int vfs_chdir(const char *path)
{
    if (!path)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    dentry_t *nd = vfs_lookup(path);
    if (!nd)
    {
        return ENO5_NOSUCH_ENTRY;
    }
    if (!nd->d_inode || !S_ISDIR(nd->d_inode->i_mode))
    {
        dentry_put(nd);
        return ENO9_NOT_DIR;
    }

    /* 释放旧的工作目录引用，持有新的 */
    pcb_t *cur_proc = getCurrentProc();
    if (cur_proc && cur_proc->proc_cwd)
    {
        dentry_put(cur_proc->proc_cwd);
    }
    if (cur_proc)
    {
        cur_proc->proc_cwd = nd;  /* nd 的引用计数已 +1，此处转移所有权 */
    }

    return ENO0_NO_ERROR;
}

/**
 * @brief 获取当前进程的工作目录路径
 * @param[out] buf  存放路径字符串的缓冲区
 * @param[in]  size 缓冲区大小
 * @retval ENO0_NO_ERROR     成功，buf 填入以 '/' 开头的绝对路径
 * @retval ENO8_NULL_POINTER 参数为 NULL 或 size 为 0
 * @retval ENO11_NAME_TOO_LONG 缓冲区太小
 * @note 算法：从 proc_cwd 逐级向上回溯至根，逆序拼接各分量。
 */
int vfs_getcwd(char *buf, size_t size)
{
    if (!buf || size == 0)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    /* 若 cwd 未设置或等于根，直接返回 "/" */
    pcb_t *cur_proc = getCurrentProc();
    dentry_t *cwd = (cur_proc && cur_proc->proc_cwd)
                    ? cur_proc->proc_cwd
                    : vfs_root_dentry;

    if (cwd == vfs_root_dentry)
    {
        if (size < 2)
        {
            return ENO11_NAME_TOO_LONG;
        }
        buf[0] = '/';
        buf[1] = '\0';
        return ENO0_NO_ERROR;
    }

    /* 从 cwd 向上回溯，收集路径分量 */
    /* 用临时数组保存各分量指针（最多 64 级深度）*/
    const char *parts[64];
    int depth = 0;

    dentry_t *d = cwd;
    while (d != vfs_root_dentry && d->d_parent != d && depth < 64)
    {
        parts[depth++] = d->d_name;
        d = d->d_parent;
    }

    /* 从后向前拼接路径 */
    char tmp[VFS_PATH_MAX];
    int pos = 0;
    for (int i = depth - 1; i >= 0; i--)
    {
        tmp[pos++] = '/';
        const char *p = parts[i];
        while (*p && pos < VFS_PATH_MAX - 1)
        {
            tmp[pos++] = *p++;
        }
    }
    tmp[pos] = '\0';

    if (pos == 0)
    {
        tmp[0] = '/';
        tmp[1] = '\0';
        pos = 1;
    }

    if ((size_t)(pos + 1) > size)
    {
        return ENO11_NAME_TOO_LONG;
    }
    memcpy(buf, tmp, pos + 1);
    return ENO0_NO_ERROR;
}

/* ============================================================
 * 文件元数据查询
 * ============================================================ */

/**
 * @brief 获取文件/目录的元数据
 * @param[in]  path    目标路径
 * @param[out] statbuf 接收结果的 stat_t 结构体
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO8_NULL_POINTER 参数为 NULL
 * @retval ENO5_NOSUCH_ENTRY 路径不存在
 */
int vfs_stat(const char *path, stat_t *statbuf)
{
    if (!path || !statbuf)
    {
        return ENO8_NULL_POINTER;
    }
    if (!vfs_root_dentry)
    {
        return ENO13_NO_FS;
    }

    dentry_t *d = vfs_lookup(path);
    if (!d)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    inode_t *inode = d->d_inode;
    if (!inode)
    {
        dentry_put(d);
        return ENO5_NOSUCH_ENTRY;
    }

    statbuf->st_ino   = inode->i_ino;
    statbuf->st_mode  = inode->i_mode;
    statbuf->st_size  = inode->i_size;
    statbuf->st_nlink = 1;  /* FAT 文件系统不支持硬链接，固定为 1 */

    dentry_put(d);
    return ENO0_NO_ERROR;
}
