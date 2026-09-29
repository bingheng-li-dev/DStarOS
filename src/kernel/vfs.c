/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

/*
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
#include "slab.h"
#include "kmalloc.h"
#include "errorcode.h"
#include "sync.h"
#include "stringops.h"
#include "proc.h"
#include "cpu.h"
#include "console.h"

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

/* VFS 大锁：保护 dentry/inode 缓存树与 FatFS 卷内部状态，调用方在"进入 VFS 的入口"
 * 处持有（syscall.c 的文件 syscall、proc.c 的 do_exec/fd 关闭路径等），vfs.c 内部
 * 不再重复加锁。用睡眠信号量而非自旋锁——FatFS 一次调用链可能连续访问多个扇区，
 * 自旋锁会整段关中断，破坏调度。 */
static ossem_t vfs_big_lock;

/* 当前持有 vfs_big_lock 的进程，NULL 表示无人持有。只用于让内存压力回调
 * （vfs_dcache_reclaim）判断"此刻碰 dentry 树安不安全"——ossem_t 没有 trydown，
 * 从任意分配失败点直接 sem_down 会在已持锁的进程里自锁死。 */
static pcb_t *vfs_lock_owner = NULL;

/* VFS 全局根目录项与根挂载点（由 vfs_mount("/", ...) 设置）*/
dentry_t   *vfs_root_dentry = NULL;
vfsmount_t *vfs_root_mount  = NULL;

/* ============================================================
 * 目录项缓存（dcache）全局状态
 *
 * 引用归零的目录项不再立即释放，而是挂到这条 LRU 上：对象仍然活着、仍挂在
 * 父目录的 d_subdirs 里，下一次 dentry_lookup 能直接命中并"复活"，省掉一整趟
 * i_op->lookup（在 FatFS 上就是一次 f_stat，SD 卡上是一次真实的扇区读）。
 *
 * 保护：沿用 vfs_big_lock，不引入新锁——所有增删都发生在 dentry_get/dentry_put
 * 里，vfs.c 内部与 proc.c 的 fork / exit 路径都在大锁之内调用它们。
 * ============================================================ */

/* LRU 链表（哨兵头）：头部最近使用，尾部最久未使用，从尾部回收 */
static struct list_head dcache_lru;

static uint64_t dcache_hits;
static uint64_t dcache_misses;
static uint64_t dcache_revives;
static uint64_t dcache_evicts;
static uint32_t dcache_nr_unused;

/* ============================================================
 * 内部工具函数：路径字符串操作
 * ============================================================ */

/**
 * @brief 从路径字符串中提取下一个分量
 * @param[in,out] path    指向路径当前位置的指针（解析后向前推进）
 * @retval >0 分量长度
 * @retval  0 路径已结束
 * @retval <0 分量名过长（ENO11_NAME_TOO_LONG）
 */
static int path_next_component(const char **path, char *name, int namelen)
{
    while (**path == '/')
    {
        (*path)++;
    }

    if (**path == '\0')
    {
        return 0;
    }

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
 * @param[out] name   输出文件名缓冲区（如 "file.txt"）
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

/* 把目录项从 LRU 上摘下来；返回它原本是否在 LRU 上 */
static bool dcache_lru_del(dentry_t *d)
{
    if (list_empty(&d->d_lru))
    {
        return false;
    }
    list_del_init(&d->d_lru);
    dcache_nr_unused--;
    return true;
}

/**
 * @brief 判断一个目录项是否值得放进 LRU 缓存
 * @details 两类不值得：
 *   -# 已脱链（d_parent 指向自身）——unlink/rmdir 过的目录项永远不可能再被
 *      dentry_lookup 命中，缓存它纯属浪费；文件系统局部根同样满足这个条件，
 *      但它的引用被 vfs_root_dentry/挂载点钉着，正常情况下走不到这里。
 *   -# 负目录项（d_inode == NULL）——只在 create/mkdir/rename 途中临时存在，
 *      缓存"不存在"需要一整套逐出规则，目前不做。
 */
static bool dcache_should_cache(dentry_t *d)
{
    return d->d_parent != d && d->d_inode != NULL;
}

/**
 * @brief 真正销毁一个引用已归零的目录项，并沿 d_parent 向上归还父引用
 * @param[in] d 待销毁的目录项，调用者保证 d->d_ref == 0
 * @details 依次：从 LRU 摘除、调 d_release 回调、从父目录子链表脱链、销毁 inode、
 *   释放名称与描述符本身，最后归还它对父目录持有的那个引用（见 dentry_create()）。
 *   父目录可能因此归零：若值得缓存就进 LRU 并到此为止，否则继续向上销毁。
 *
 *   写成沿 d_parent 向上的循环而不是递归——内核栈只有 KERNEL_STACKPSIZE 页，
 *   深路径递归会踩爆。
 * @note 这里是唯一归还父引用的地方；dentry_put 引用归零转入 LRU 时不归还
 *   （否则父目录会在子项还缓存着的时候被提前释放）。
 * @note 由"LRU 上的目录项一定是叶子"可知：本函数除了可能往 LRU 头部插入
 *   一个父目录之外，绝不会释放链表上的其它条目——dcache_shrink_to 和
 *   dcache_prune_subtree 的遍历安全性都建立在这条性质上。
 */
static void dcache_evict(dentry_t *d)
{
    while (d != NULL)
    {
        dcache_lru_del(d);

        if (d->d_op && d->d_op->d_release)
        {
            d->d_op->d_release(d);
        }

        /* 文件系统局部根的 d_parent 指向自身，不参与父引用 */
        dentry_t *parent = (d->d_parent != d) ? d->d_parent : NULL;
        if (parent != NULL)
        {
            list_del_init(&d->d_child);
        }

        /* inode 与 dentry 是一对一的，dentry 消失时 inode 必须一起回收，
         * 否则每解析一次路径就漏掉一个 inode。 */
        if (d->d_inode != NULL)
        {
            destroy_inode(d->d_inode);
            d->d_inode = NULL;
        }

        kfree(d->d_name);
        kfree(d);
        dcache_evicts++;

        d = NULL;
        if (parent != NULL && parent->d_ref > 0)
        {
            parent->d_ref--;
            if (parent->d_ref == 0)
            {
                if (dcache_should_cache(parent))
                {
                    list_add(&parent->d_lru, &dcache_lru);
                    dcache_nr_unused++;
                }
                else
                {
                    d = parent;
                }
            }
        }
    }
}

/**
 * @brief 从 LRU 尾部批量回收，直到条目数降到 target
 * @details 每轮取尾部（最久未使用）的一条真正释放。dcache_evict 归还父引用时可能
 *   把刚变成叶子的父目录插到 LRU 头部，于是 nr_unused 在循环中途是会回升的——
 *   但循环仍然一定收敛：每一轮至少 kfree 掉一个目录项，而缓存中活着的目录项
 *   数量有限且严格递减，被插进来的父目录本身也已经是叶子、迟早轮到它。
 */
static void dcache_shrink_to(uint32_t target)
{
    while (dcache_nr_unused > target && !list_empty(&dcache_lru))
    {
        dentry_t *victim = list_entry(dcache_lru.prev, dentry_t, d_lru);
        dcache_lru_del(victim);
        dcache_evict(victim);
    }
}

/**
 * @brief 判断 d 是否为 ancestor 的后代
 * @note guard 只是防御 d_parent 意外成环，正常目录树走不满。
 */
static bool dcache_is_descendant(dentry_t *d, dentry_t *ancestor)
{
    int guard = VFS_PATH_MAX;

    while (d != NULL && d->d_parent != d && guard-- > 0)
    {
        d = d->d_parent;
        if (d == ancestor)
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 把 ancestor 子树下所有仍在 LRU 上的目录项真正释放（ancestor 本身不动）
 * @param[in] ancestor 子树根，调用者必须持有它的一个引用
 * @details unmount 这一处是非用不可的：LRU 上的目录项持有指向该文件系统 inode
 *   的指针，而 inode 又指向马上要被 destroy_super_block 销毁的超级块。不清干净
 *   就是一批悬空引用。正在被使用的（d_ref > 0）动不了，那是卸载忙碌文件系统本身
 *   的问题，不在本处理范围。
 *
 *   rmdir 那一处是防御性的：缓存里只会有磁盘上真实存在的条目，所以判空看到子项
 *   就是真非空，本身并不会误判；剪一遍是为了让 list_empty(&d_subdirs) 的语义
 *   收窄成"还有人在用的子项"，日后真加了负目录项缓存不至于悄悄变成误报。
 *
 *   rename 不需要剪：路径不缓存，目录项挂到新父目录后整棵子树的路径自然跟着变。
 *
 *   用"反复扫 LRU"而不是递归下降——内核栈只有一页，目录深度不可控。每一轮至多
 *   释放掉当前这层的叶子，它们的父目录归零后进入 LRU，下一轮再被扫到，
 *   轮数不超过子树深度。
 */
static void dcache_prune_subtree(dentry_t *ancestor)
{
    bool progress = true;

    while (progress)
    {
        struct list_head *pos, *tmp;

        progress = false;
        list_for_each_safe(pos, tmp, &dcache_lru)
        {
            dentry_t *d = list_entry(pos, dentry_t, d_lru);
            if (!dcache_is_descendant(d, ancestor))
            {
                continue;
            }
            dcache_lru_del(d);
            dcache_evict(d);
            progress = true;
        }
    }
}

/**
 * @brief 增加目录项引用计数（内部使用）
 * @note 引用从 0 提到 1 时必须把它从 LRU 摘掉，否则一个正在被使用的目录项还挂在
 *   回收链上，下一次 shrink 会把它释放掉。这里与 dentry_put 的入队严格成对——
 *   放在 dentry_get 而不是只放在 dentry_lookup 里，是为了让配对关系是结构性的：
 *   ".." 上溯、跨挂载点、fork 复制 cwd 等路径都各自调 dentry_get，逐个记得加
 *   一句"复活"迟早会漏。
 */
static void dentry_get(dentry_t *d)
{
    if (d)
    {
        if (d->d_ref == 0 && dcache_lru_del(d))
        {
            dcache_revives++;
        }
        d->d_ref++;
    }
}

/**
 * @brief 减少目录项引用计数，归零时转入 LRU 缓存（内部使用）
 * @details 引用归零不等于释放：值得缓存的目录项挂到 dcache_lru 头部就返回，
 *   对象继续活着、继续挂在父目录的 d_subdirs 里、继续能被 dentry_lookup 命中，
 *   下一次访问同一路径就省掉一趟 i_op->lookup。真正的释放推迟到水位线触发的
 *   dcache_shrink_to()，或 dcache_prune_subtree()。不值得缓存的（已脱链、
 *   负目录项）直接走 dcache_evict()。
 * @note d_ref 的含义是"外部持有者数量 + 子目录项数量"。外部持有者包括
 *   vfs_lookup() 返回给调用者的那一个、proc_cwd、file_t.f_dentry、
 *   以及挂载点的 vfsmount.mnt_host_dentry。
 * @note 转入 LRU 时不归还对父目录的引用——缓存一个叶子会顺带把它整条祖先链
 *   钉在内存里，这正是想要的（祖先目录本来就最该缓存），也保证了 LRU 上的目录项
 *   永远不会有一个已被释放的 d_parent。归还统一由 dcache_evict() 负责。
 */
static void dentry_put(dentry_t *d)
{
    if (d == NULL || d->d_ref == 0)
    {
        return; /* 防御：引用已归零的目录项不应再被释放 */
    }

    d->d_ref--;
    if (d->d_ref > 0)
    {
        return;
    }

    if (!dcache_should_cache(d))
    {
        dcache_evict(d);
        return;
    }

    list_add(&d->d_lru, &dcache_lru);   /* 头插 = 最近使用 */
    dcache_nr_unused++;
    if (dcache_nr_unused > DCACHE_MAX_UNUSED)
    {
        dcache_shrink_to(DCACHE_LOW_WATER);
    }
}

/**
 * @brief 把目录项从父目录的子链表中摘除，并释放它对父目录持有的引用
 * @details 用于 unlink/rmdir/rename——这些操作必须让后续 lookup 看不到这个
 *   目录项，不能等到引用归零才摘。脱链后 d_parent 指向自身，既标记"已脱链"
 *   使重复调用无副作用，也让仍持有它的进程做 ".." 时原地不动而不是解引用悬空指针。
 */
static void dentry_detach(dentry_t *d)
{
    if (!d || d->d_parent == d || d->d_parent == NULL)
    {
        return;
    }
    dentry_t *parent = d->d_parent;
    list_del_init(&d->d_child);
    d->d_parent = d;
    dentry_put(parent);
}

/**
 * @brief 引用计数 +1（供 proc.c 等外部模块使用）
 */
void vfs_dentry_get(dentry_t *d)
{
    dentry_get(d);
}

/**
 * @brief 引用计数 -1，归零时转入 LRU 或回收（供外部模块使用）
 */
void vfs_dentry_put(dentry_t *d)
{
    dentry_put(d);
}

/**
 * @brief 回收至多 nr 条缓存目录项，供内存压力路径调用
 * @param[in] nr 期望回收的条目数
 * @note 目前只有自检用例调用；内存压力路径走 vfs_dcache_reclaim()。
 * @note 调用者必须已持有 vfs 大锁（vfs_lock）——本函数会改动 dentry 树。
 */
void vfs_dcache_shrink(uint32_t nr)
{
    uint32_t target = (dcache_nr_unused > nr) ? (dcache_nr_unused - nr) : 0;
    dcache_shrink_to(target);
}

/**
 * @brief 内存不足时的目录项缓存回收回调，由 kmalloc 的重试路径调用
 * @details 缓存目录项是真正可以丢弃的数据，比 slab 的空闲页更该先吐出来，
 *   所以 kmalloc 重试时先调本函数、再调 slab_reclaim_all()。压力来临时不留情面，
 *   直接清空整条 LRU。
 * @note 只在调用者恰好是 vfs_big_lock 的持有者时才真的干活。dentry 树由大锁
 *   保护，而 kmalloc 可能在任何上下文（含持自旋锁、含根本没进过 VFS 的路径）里
 *   失败；ossem_t 没有 trydown，就地 sem_down 要么自锁死要么在关中断状态下睡眠。
 *   好在最需要它的场合恰好满足这个条件——正是在 VFS 调用内部创建 dentry/inode
 *   时把内存耗光的那一次。其余场合退化成空操作，交给 slab_reclaim_all()。
 */
void vfs_dcache_reclaim(void)
{
    if (vfs_lock_owner == NULL || vfs_lock_owner != proc_get_current())
    {
        return;
    }
    dcache_shrink_to(0);
}

/**
 * @brief 取目录项缓存统计快照
 * @param[out] out 接收统计值的结构体
 */
void vfs_dcache_get_stats(dcache_stats_t *out)
{
    if (!out)
    {
        return;
    }
    out->hits      = dcache_hits;
    out->misses    = dcache_misses;
    out->revives   = dcache_revives;
    out->evicts    = dcache_evicts;
    out->nr_unused = dcache_nr_unused;
}

/**
 * @brief 打印目录项缓存统计
 */
void vfs_dcache_stats(void)
{
    uint64_t total = dcache_hits + dcache_misses;
    uint64_t rate  = total ? (dcache_hits * 100 / total) : 0;

    printf("dcache: hit=%ld miss=%ld (%ld%%) revive=%ld evict=%ld unused=%d/%d\n",
           dcache_hits, dcache_misses, rate, dcache_revives, dcache_evicts,
           dcache_nr_unused, DCACHE_MAX_UNUSED);
}

/* ============================================================
 * 文件系统类型注册与注销
 * ============================================================ */

/**
 * @brief 在已注册链表中查找文件系统类型
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

    irq_key_t vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);

    int nlen = (int)strlen(fs_type->name);
    fs_type_ptr = find_filesystem_by_name(fs_type->name, nlen);
    if (*fs_type_ptr)
    {
        ret = ENO4_BUSY;
    }
    else
    {
        *fs_type_ptr = fs_type;
    }

    spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

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

    irq_key_t vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);

    while (*fs_type_ptr)
    {
        if (*fs_type_ptr == fs_type)
        {
            *fs_type_ptr = fs_type->next;
            fs_type->next = NULL;
            spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);
            return ENO0_NO_ERROR;
        }
        fs_type_ptr = &(*fs_type_ptr)->next;
    }

    spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

    return ENO5_NOSUCH_ENTRY;
}

/* 按名称取已注册的文件系统类型，未找到返回 NULL */
static file_system_type_t *get_fs_type_by_name(const char *name)
{
    if (!name)
    {
        return NULL;
    }

    int nlen = (int)strlen(name);
    irq_key_t vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);
    file_system_type_t *fs = *find_filesystem_by_name(name, nlen);
    spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

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
 * @note 只摘链 + 释放超级块本身。根 dentry / 根 inode 不在这里回收——它们的
 *   destroy_inode 要经 inode->i_sb->s_op 分发，必须赶在本函数之前放掉，
 *   见 vfs_unmount() 里的处理。调用前应先完成文件系统的卸载（unmount）。
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
 * @brief 通过超级块的 destroy_inode 回调释放 inode
 * @param[in] inode 要释放的 inode 指针
 */
void destroy_inode(inode_t *inode)
{
    if (!inode || !inode->i_sb || !inode->i_sb->s_op ||
        !inode->i_sb->s_op->destroy_inode)
    {
        return;
    }
    inode->i_sb->s_op->destroy_inode(inode);
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

    dentry_t *d = (dentry_t *)slab_cache_alloc(dentry_cache);
    if (!d)
    {
        return NULL;
    }

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

    INIT_LIST_HEAD(&d->d_subdirs);
    /* 孤立的 d_lru 节点表示"不在 LRU 上"，d_ref 从 1 起步本来就不该在 LRU 上 */
    INIT_LIST_HEAD(&d->d_lru);

    if (!parent)
    {
        /* 文件系统局部根：父指向自身，d_child 为孤立节点，不挂到任何父链表 */
        d->d_parent = d;
        INIT_LIST_HEAD(&d->d_child);
    }
    else
    {
        /* 子目录项持有父目录的一个引用：否则 vfs_lookup 逐分量前进时对中间分量做的
         * dentry_put 会把刚当上父节点的目录项直接释放掉，留下 d_parent 悬空、
         * d_child 挂在已释放内存里的子节点。 */
        d->d_parent = parent;
        dentry_get(parent);
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
            dentry_get(child);   /* 命中的若在 LRU 上，由 dentry_get 负责复活 */
            dcache_hits++;
            return child;
        }
    }
    dcache_misses++;
    return NULL;
}

/* 找挂载了 sb 的 vfsmount；调用者持 vfs_fs_lock */
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
 * @note 必须早于任何文件系统注册与挂载。
 */
void vfs_init(void)
{
    INIT_LIST_HEAD(&super_block_list);
    INIT_LIST_HEAD(&vfs_mount_list);
    INIT_LIST_HEAD(&dcache_lru);
    dcache_hits      = 0;
    dcache_misses    = 0;
    dcache_revives   = 0;
    dcache_evicts    = 0;
    dcache_nr_unused = 0;
    spinlock_init(&vfs_fs_lock);
    sem_init(&vfs_big_lock, 1);
    file_system_types = NULL;
    vfs_root_dentry   = NULL;
    vfs_root_mount    = NULL;
}

/**
 * @brief 获取 VFS 大锁
 * @note 只在"进入 VFS 的入口"调用（syscall.c 的文件 syscall、proc.c 的 do_exec/
 *   fd 关闭路径等），不要在 vfs.c 内部函数之间嵌套调用——ossem_t 不可重入，
 *   vfs_open 内部会调 vfs_lookup，嵌套加锁必然自锁死。
 */
void vfs_lock(void)
{
    sem_down(&vfs_big_lock);
    vfs_lock_owner = proc_get_current();
}

/**
 * @brief 释放 VFS 大锁
 */
void vfs_unlock(void)
{
    vfs_lock_owner = NULL;
    sem_up(&vfs_big_lock);
}

/* ============================================================
 * 核心路径解析：vfs_lookup
 * ============================================================ */

/**
 * @brief 若 d 是某文件系统的挂载点，穿越进被挂载文件系统的根；否则原样返回
 * @param[in] d 待检查的目录项，持有一个引用计数
 * @return 穿越后的目录项（可能是被挂载文件系统的根，也可能是形参 d 本身），
 *   同样持有一个引用计数——调用者不需要关心是否发生了穿越，统一按"消费掉传入的
 *   引用、拿到一个新的引用"来处理
 * @note vfs_lookup() 里两处调用：循环内处理下一个分量之前、以及分量耗尽之后。后者不能省，
 *   否则路径恰好是挂载点（如 "/dev"）时返回的是宿主那个空目录，不是被挂载文件系统的根。
 */
static dentry_t *cross_mountpoints(dentry_t *d)
{
    while (d->d_mounted)
    {
        vfsmount_t *mnt = d->d_mounted;
        if (!mnt->mnt_sb || !mnt->mnt_sb->s_root_inode ||
            !mnt->mnt_sb->s_root_inode->i_dentry)
        {
            break;
        }
        dentry_t *mnt_root = mnt->mnt_sb->s_root_inode->i_dentry;
        dentry_get(mnt_root);
        dentry_put(d);
        d = mnt_root;
    }
    return d;
}

/* 解析起点：绝对路径从全局根开始（并跳过开头的 '/'），相对路径从 cwd 开始。返回持有一个引用的 dentry */
static dentry_t *lookup_start(const char **path)
{
    dentry_t *start = vfs_root_dentry;
    if (**path == '/')
    {
        while (**path == '/')
        {
            (*path)++;
        }
    }
    else
    {
        pcb_t *cur_proc = proc_get_current();
        if (cur_proc && cur_proc->proc_cwd)
        {
            start = cur_proc->proc_cwd;
        }
    }
    dentry_get(start);
    return start;
}

/* 走一步 ".."，接管 cur 的引用、返回持有一个引用的新位置。
 * 已在全局根则原地不动；在文件系统局部根（d_parent 指向自身）上要反向穿过挂载点，
 * 取宿主目录项的父节点——找不到宿主就留在局部根（理论上不应发生）。 */
static dentry_t *lookup_dotdot(dentry_t *cur)
{
    if (cur == vfs_root_dentry)
    {
        return cur;
    }

    dentry_t *parent = cur->d_parent;
    if (parent == cur)
    {
        vfsmount_t *mnt = NULL;
        irq_key_t vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);
        if (cur->d_inode && cur->d_inode->i_sb)
        {
            mnt = find_mount_by_sb(cur->d_inode->i_sb);
        }
        spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

        if (!mnt || !mnt->mnt_host_dentry || !mnt->mnt_host_dentry->d_parent)
        {
            return cur;
        }
        parent = mnt->mnt_host_dentry->d_parent;
    }
    dentry_get(parent);
    dentry_put(cur);
    return parent;
}

/**
 * @brief 将路径字符串解析为对应的目录项
 * @param[in] path 要解析的路径字符串（绝对或相对）
 * @return 持有一个引用计数的目录项；路径不存在或出错返回 NULL
 * @note 调用者负责调用 vfs_dentry_put() 释放返回的引用。
 * @details 解析算法：
 *  -# 若路径以 '/' 开头，起始节点为 vfs_root_dentry；否则为 proc_cwd（NULL 时回退到根）。
 *  -# 逐分量循环：
 *     - 进入节点前检查 d_mounted，穿越到被挂载文件系统的根；
 *     - "." 保持不动；".." 向上，若已是文件系统局部根则反向穿越挂载点；
 *     - 普通分量先查内存缓存（dentry_lookup），未命中则调 i_op->lookup。
 *  -# 循环结束（分量耗尽）后再做一次同样的穿越检查——见 cross_mountpoints() 的说明。
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

    dentry_t *cur = lookup_start(&path);

    /* 纯 "/" 路径或空相对路径：直接返回当前节点 */
    if (*path == '\0')
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
            dentry_put(cur);
            return NULL;
        }

        cur = cross_mountpoints(cur);

        if (comp[0] == '.' && comp[1] == '\0')
        {
            continue;
        }

        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0')
        {
            cur = lookup_dotdot(cur);
            continue;
        }

        if (!cur->d_inode || !S_ISDIR(cur->d_inode->i_mode))
        {
            dentry_put(cur);
            return NULL;
        }

        /* 先查内存缓存（dentry_lookup 已包含 dentry_get）*/
        dentry_t *next = dentry_lookup(cur, comp);

        if (!next)
        {
            if (!cur->d_inode->i_op || !cur->d_inode->i_op->lookup)
            {
                dentry_put(cur);
                return NULL;
            }
            next = cur->d_inode->i_op->lookup(cur->d_inode, comp);
        }

        if (!next)
        {
            dentry_put(cur);
            return NULL;
        }

        dentry_put(cur);
        cur = next;
    }

    /* 路径恰好在挂载点结束时也要穿越，见 cross_mountpoints() */
    return cross_mountpoints(cur);
}

/* ============================================================
 * 挂载与卸载
 * ============================================================ */

/* 挂载回调成功之后的失败出口：按 vfs_unmount 的顺序拆掉回调建好的根目录项、根 inode
 * 与超级块。根目录项还被子项引用着（devfs 挂载时就建好了子项）时拆不得——超级块一销毁，
 * 那些 inode 的 i_sb 就成了悬空指针——只能留着。 */
static void vfs_mount_undo(dentry_t *root)
{
    super_block_t *sb = (root->d_inode != NULL) ? root->d_inode->i_sb : NULL;

    if (root->d_ref != 1)
    {
        return;
    }
    dentry_put(root);
    if (sb != NULL && sb->s_op != NULL && sb->s_op->unmount != NULL)
    {
        sb->s_op->unmount(sb);
    }
    if (sb != NULL)
    {
        destroy_super_block(sb);
    }
}

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

    file_system_type_t *fst = get_fs_type_by_name(fs_type);
    if (!fst)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    dentry_t *root_dentry = fst->mount(fst, path, data);
    if (!root_dentry)
    {
        return ENO13_NO_FS;
    }

    vfsmount_t *mnt = (vfsmount_t *)kmalloc(sizeof(vfsmount_t));
    if (!mnt)
    {
        vfs_mount_undo(root_dentry);
        return ENO1_NOMORE_MEM;
    }

    int plen = (int)strlen(path);
    mnt->mnt_path = (char *)kmalloc(plen + 1);
    if (!mnt->mnt_path)
    {
        kfree(mnt);
        vfs_mount_undo(root_dentry);
        return ENO1_NOMORE_MEM;
    }
    memcpy(mnt->mnt_path, path, plen + 1);

    /* 挂载回调理应已建好超级块；根 dentry 没有 inode 时 mnt_sb 记 NULL */
    mnt->mnt_sb = root_dentry->d_inode ? root_dentry->d_inode->i_sb : NULL;
    mnt->mnt_host_dentry = NULL;
    INIT_LIST_HEAD(&mnt->mnt_list_linker);

    if (path[0] == '/' && path[1] == '\0')
    {
        vfs_root_dentry = root_dentry;
        vfs_root_mount  = mnt;
        mnt->mnt_host_dentry = NULL;
    }
    else
    {
        dentry_t *host = vfs_lookup(path);
        if (!host)
        {
            kfree(mnt->mnt_path);
            kfree(mnt);
            vfs_mount_undo(root_dentry);
            return ENO5_NOSUCH_ENTRY;
        }
        if (!host->d_inode || !S_ISDIR(host->d_inode->i_mode))
        {
            dentry_put(host);
            kfree(mnt->mnt_path);
            kfree(mnt);
            vfs_mount_undo(root_dentry);
            return ENO9_NOT_DIR;
        }
        if (host->d_mounted)
        {
            dentry_put(host);
            kfree(mnt->mnt_path);
            kfree(mnt);
            vfs_mount_undo(root_dentry);
            return ENO4_BUSY;
        }
        host->d_mounted      = mnt;
        mnt->mnt_host_dentry = host;
        /* 持有宿主 dentry 的引用（挂载期间不允许删除）*/
    }

    if (mnt->mnt_sb)
    {
        mnt->mnt_sb->s_mount = mnt;
    }

    irq_key_t vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);
    list_add(&mnt->mnt_list_linker, &vfs_mount_list);
    spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

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

    if (path[0] == '/' && path[1] == '\0')
    {
        return ENO16_PERM;
    }

    vfsmount_t *target = NULL;
    irq_key_t vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);
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
    spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

    if (!target)
    {
        return ENO5_NOSUCH_ENTRY;
    }

    dentry_t *fs_root = (target->mnt_sb && target->mnt_sb->s_root_inode)
                        ? target->mnt_sb->s_root_inode->i_dentry
                        : NULL;

    /* 先把这个文件系统里纯缓存的目录项清干净：它们的 inode 指向马上要被销毁的
     * 超级块，留在 LRU 上就是一批悬空引用。 */
    if (fs_root)
    {
        dcache_prune_subtree(fs_root);
    }

    /* 剪枝之后根目录项若还剩不止 dentry_create 那一个引用，说明这个文件系统里仍有
     * 活着的目录项（子项各持父目录一个引用）或外部持有者（cwd、打开的文件）。
     * 此时必须拒绝：超级块一销毁，那些还活着的 inode 的 i_sb 就成了悬空指针，
     * 比泄漏严重得多。devfs 会走到这里——它没有 i_op->lookup，只能靠创建时的引用
     * 把四个设备条目钉住不让逐出，根引用数因此恒 > 1，暂时不支持卸载。 */
    if (fs_root && fs_root->d_ref != 1)
    {
        return ENO4_BUSY;
    }

    if (target->mnt_sb && target->mnt_sb->s_op && target->mnt_sb->s_op->sync_fs)
    {
        target->mnt_sb->s_op->sync_fs(target->mnt_sb);
    }

    /* 归还根目录项创建时的那一个引用。文件系统局部根的 d_parent 指向自身，
     * dcache_should_cache() 因此为假，dentry_put 会直接走 dcache_evict——连带经
     * s_op->destroy_inode 回收根 inode。两件事的先后不能反：destroy_inode 要经
     * inode->i_sb->s_op 分发，超级块必须还活着；而底层文件系统的 unmount 回调
     * 可能拆掉 destroy_inode 依赖的状态，所以也放在它后面。 */
    if (fs_root)
    {
        dentry_put(fs_root);
    }

    if (target->mnt_sb && target->mnt_sb->s_op && target->mnt_sb->s_op->unmount)
    {
        target->mnt_sb->s_op->unmount(target->mnt_sb);
    }

    if (target->mnt_host_dentry)
    {
        target->mnt_host_dentry->d_mounted = NULL;
        dentry_put(target->mnt_host_dentry);
    }

    if (target->mnt_sb)
    {
        destroy_super_block(target->mnt_sb);
    }

    vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);
    list_del(&target->mnt_list_linker);
    spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

    kfree(target->mnt_path);
    kfree(target);

    return ENO0_NO_ERROR;
}

/* ============================================================
 * 文件操作
 * ============================================================ */

/* vfs_open 的失败出口：err 非 NULL 时写入 code，恒返回 NULL */
static file_t *vfs_open_fail(int *err, int code)
{
    if (err != NULL)
    {
        *err = code;
    }
    return NULL;
}

/* O_CREAT 且目标不存在：在父目录里建一个普通文件，*out 得到持有一个引用的新 dentry */
static int vfs_create_regular(const char *path, dentry_t **out)
{
    char ppath[VFS_PATH_MAX], fname[VFS_NAME_MAX];
    if (split_path(path, ppath, VFS_PATH_MAX, fname, VFS_NAME_MAX) < 0)
    {
        return ENO11_NAME_TOO_LONG;
    }

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

    dentry_t *new_d = dentry_create(fname, NULL, parent, NULL);
    if (!new_d)
    {
        dentry_put(parent);
        return ENO1_NOMORE_MEM;
    }

    if (!parent->d_inode->i_op || !parent->d_inode->i_op->create)
    {
        dentry_put(new_d);
        dentry_put(parent);
        return ENO16_PERM;
    }

    int ret = parent->d_inode->i_op->create(parent->d_inode, new_d, S_IFREG | 0644);
    dentry_put(parent);
    if (ret != ENO0_NO_ERROR)
    {
        dentry_put(new_d);
        return ret;
    }
    *out = new_d;
    return ENO0_NO_ERROR;
}

/* 目录只允许只读打开（供 getdents64 遍历），O_TRUNC 也要挡——截断一个目录没有意义，
 * 放过去只会让底层拿到矛盾的语义；反过来，带 O_DIRECTORY 却指向普通文件按 POSIX 是 ENOTDIR */
static int vfs_check_open_mode(const inode_t *inode, int mode)
{
    if (S_ISDIR(inode->i_mode))
    {
        int acc = mode & O_ACCMODE;
        if (acc == O_WRONLY || acc == O_RDWR || (mode & O_TRUNC))
        {
            return ENO10_IS_DIR;
        }
    }
    else if (mode & O_DIRECTORY)
    {
        return ENO9_NOT_DIR;
    }
    return ENO0_NO_ERROR;
}

/* 为 target 造一个新的 file_t，接管调用方持有的那个 dentry 引用；内存不足返回 NULL */
static file_t *vfs_file_alloc(dentry_t *target, const char *path, int mode)
{
    file_t *file = (file_t *)slab_cache_alloc(file_cache);
    if (!file)
    {
        return NULL;
    }

    /* 复制路径字符串（调试用）*/
    int pathlen = (int)strlen(path);
    file->f_path = (char *)kmalloc(pathlen + 1);
    if (!file->f_path)
    {
        kfree(file);
        return NULL;
    }
    memcpy(file->f_path, path, pathlen + 1);

    file->f_inode   = target->d_inode;
    file->f_dentry  = target;
    file->f_op      = target->d_inode->i_fop;
    file->f_mode    = mode;
    file->f_count   = 1;
    file->f_private = NULL;
    file->f_vfsmount = NULL;
    file->f_kind    = FILE_KIND_VFS;
    file->f_pos     = (mode & O_APPEND) ? (off_t)target->d_inode->i_size : 0;
    return file;
}

/**
 * @brief 打开（或创建）文件
 * @param[in] path 文件路径
 * @param[in] mode 打开模式标志（O_RDONLY/O_WRONLY/O_RDWR/O_CREAT 等）
 * @param[out] err  非 NULL 时写入结果：成功为 ENO0_NO_ERROR，失败为负的 ENO* 错误码
 * @return 成功返回打开的 file_t 指针；失败返回 NULL，原因见 err
 */
file_t *vfs_open(const char *path, int mode, int *err)
{
    if (!path)
    {
        return vfs_open_fail(err, ENO8_NULL_POINTER);
    }
    if (!vfs_root_dentry)
    {
        return vfs_open_fail(err, ENO13_NO_FS);
    }

    int ret;
    dentry_t *target = vfs_lookup(path);
    if (!target)
    {
        if (!(mode & O_CREAT))
        {
            return vfs_open_fail(err, ENO5_NOSUCH_ENTRY);
        }
        ret = vfs_create_regular(path, &target);
        if (ret != ENO0_NO_ERROR)
        {
            return vfs_open_fail(err, ret);
        }
    }
    else if ((mode & O_CREAT) && (mode & O_EXCL))
    {
        dentry_put(target);
        return vfs_open_fail(err, ENO7_EXISTS);
    }

    /* target 现在指向有效的 dentry，且持有一个引用计数 */
    if (!target->d_inode)
    {
        dentry_put(target);
        return vfs_open_fail(err, ENO5_NOSUCH_ENTRY);
    }
    ret = vfs_check_open_mode(target->d_inode, mode);
    if (ret != ENO0_NO_ERROR)
    {
        dentry_put(target);
        return vfs_open_fail(err, ret);
    }

    file_t *file = vfs_file_alloc(target, path, mode);
    if (!file)
    {
        dentry_put(target);
        return vfs_open_fail(err, ENO1_NOMORE_MEM);
    }

    /* 调用底层 open 回调（分配底层资源，如 FIL*）*/
    if (file->f_op && file->f_op->open)
    {
        ret = file->f_op->open(target->d_inode, file, mode);
        if (ret != ENO0_NO_ERROR)
        {
            kfree(file->f_path);
            kfree(file);
            dentry_put(target);
            return vfs_open_fail(err, ret);
        }
    }

    /* O_TRUNC：打开成功后截断文件（底层 open 处理，此处更新 inode 大小）*/
    if ((mode & O_TRUNC) && !S_ISDIR(target->d_inode->i_mode))
    {
        file->f_inode->i_size = 0;
        file->f_pos = 0;
    }

    if (err != NULL)
    {
        *err = ENO0_NO_ERROR;
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

    /* fork / dup 给同一个 file_t 加引用时不持任何锁，这里必须原子地减 */
    if (atomic_add(&file->f_count, -1) > 1)
    {
        return ENO0_NO_ERROR;
    }

    if (file->f_op && file->f_op->close)
    {
        file->f_op->close(file);
    }

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

    if ((file->f_mode & O_ACCMODE) == O_RDONLY)
    {
        return ENO16_PERM;
    }

    if (!file->f_op || !file->f_op->write)
    {
        return ENO8_NULL_POINTER;
    }

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
 * @brief 按已打开的文件对象截断/扩展文件（ftruncate 用）
 * @param[in] file 已打开的文件对象
 * @param[in] size 目标大小
 * @retval ENO0_NO_ERROR 成功
 * @retval ENO10_IS_DIR  目标是目录
 * @details 直接用 file->f_inode，不经路径解析——`file->f_path` 只是调试字段，
 *   文件被 rename 之后就不再指向真身，拿它去 vfs_truncate 会截断错误的文件。
 */
int vfs_ftruncate(file_t *file, uint64_t size)
{
    if (!file || !file->f_inode)
    {
        return ENO8_NULL_POINTER;
    }

    inode_t *inode = file->f_inode;
    if (S_ISDIR(inode->i_mode))
    {
        return ENO10_IS_DIR;
    }
    if (!inode->i_op || !inode->i_op->truncate)
    {
        return ENO8_NULL_POINTER;
    }

    return inode->i_op->truncate(inode, size);
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

    if (file->f_op && file->f_op->lseek)
    {
        return file->f_op->lseek(file, offset, whence);
    }

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

    dentry_t *ex = vfs_lookup(path);
    if (ex)
    {
        dentry_put(ex);
        return ENO7_EXISTS;
    }

    char ppath[VFS_PATH_MAX], dname[VFS_NAME_MAX];
    if (split_path(path, ppath, VFS_PATH_MAX, dname, VFS_NAME_MAX) < 0)
    {
        return ENO11_NAME_TOO_LONG;
    }

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

    dentry_t *nd = dentry_create(dname, NULL, parent, NULL);
    if (!nd)
    {
        dentry_put(parent);
        return ENO1_NOMORE_MEM;
    }

    if (!parent->d_inode->i_op || !parent->d_inode->i_op->mkdir)
    {
        dentry_put(nd);
        dentry_put(parent);
        return ENO8_NULL_POINTER;
    }

    int ret = parent->d_inode->i_op->mkdir(parent->d_inode, nd,
                                            S_IFDIR | (mode & 0777));
    dentry_put(parent);
    /* 成功与否都要放掉 nd：本函数不把目录项交给调用者 */
    dentry_put(nd);
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

    if (!d->d_inode || !S_ISDIR(d->d_inode->i_mode))
    {
        dentry_put(d);
        return ENO9_NOT_DIR;
    }

    /* 判空之前先剪掉子树里纯缓存的目录项，让下面这一步看到的是"还有人在用的子项" */
    dcache_prune_subtree(d);

    if (!list_empty(&d->d_subdirs))
    {
        dentry_put(d);
        return ENO12_NOT_EMPTY;
    }

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

    int ret = parent->d_inode->i_op->rmdir(parent->d_inode, d);
    if (ret == ENO0_NO_ERROR)
    {
        /* 逐出缓存：后续 lookup 不能再看到这个已删除的目录项。inode 留给
         * dentry_put 在引用归零时销毁——此刻可能还有进程把它当作 cwd。 */
        dentry_detach(d);
    }
    dentry_put(d);  /* 释放 vfs_lookup 给的那个引用 */
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

    int ret = parent->d_inode->i_op->unlink(parent->d_inode, d);
    if (ret == ENO0_NO_ERROR)
    {
        /* 逐出缓存；inode 留给 dentry_put 在引用归零时销毁——此刻可能还有
         * 打开着的 file_t 持有这个目录项（POSIX 允许删除已打开的文件）。 */
        dentry_detach(d);
    }
    dentry_put(d);  /* 释放 vfs_lookup 给的那个引用 */
    return ret;
}

/* 磁盘上已经改名成功之后同步目录项缓存：old_d 挪到 new_parent 下、改名为 new_name。
 * 新名字要先分配好再动 old_d：分配失败时只能把 old_d 从树上摘掉（同 unlink），
 * 让之后的 lookup 回到底层按新名字重建。 */
static void rename_move_dentry(dentry_t *old_d, dentry_t *new_parent, const char *new_name)
{
    int nl = (int)strlen(new_name);
    char *new_d_name = (char *)kmalloc(nl + 1);
    if (new_d_name == NULL)
    {
        dentry_detach(old_d);
        return;
    }
    memcpy(new_d_name, new_name, nl + 1);

    /* 改父目录意味着子→父引用要跟着搬家——先给新父加引用，摘链改名之后再放掉旧父的那一份 */
    dentry_t *old_parent = (old_d->d_parent != old_d) ? old_d->d_parent : NULL;
    list_del(&old_d->d_child);
    kfree(old_d->d_name);
    old_d->d_name = new_d_name;
    old_d->d_parent = new_parent;
    dentry_get(new_parent);
    list_add(&old_d->d_child, &new_parent->d_subdirs);
    if (old_parent != NULL)
    {
        dentry_put(old_parent);
    }
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

    dentry_t *new_parent = NULL;
    dentry_t *new_d = NULL;
    int ret;
    char new_ppath[VFS_PATH_MAX], new_name[VFS_NAME_MAX];
    if (split_path(newpath, new_ppath, VFS_PATH_MAX, new_name, VFS_NAME_MAX) < 0)
    {
        ret = ENO11_NAME_TOO_LONG;
        goto out;
    }

    new_parent = vfs_lookup(new_ppath);
    if (!new_parent)
    {
        ret = ENO5_NOSUCH_ENTRY;
        goto out;
    }
    if (!new_parent->d_inode || !S_ISDIR(new_parent->d_inode->i_mode))
    {
        ret = ENO9_NOT_DIR;
        goto out;
    }
    if (old_d->d_inode && old_d->d_inode->i_sb != new_parent->d_inode->i_sb)
    {
        ret = ENO14_CROSS_DEV;
        goto out;
    }

    new_d = dentry_create(new_name, NULL, new_parent, NULL);
    if (!new_d)
    {
        ret = ENO1_NOMORE_MEM;
        goto out;
    }

    ret = ENO0_NO_ERROR;
    if (old_d->d_parent && old_d->d_parent->d_inode &&
        old_d->d_parent->d_inode->i_op &&
        old_d->d_parent->d_inode->i_op->rename)
    {
        ret = old_d->d_parent->d_inode->i_op->rename(
                  old_d->d_parent->d_inode, old_d,
                  new_parent->d_inode, new_d);
    }
    if (ret == ENO0_NO_ERROR)
    {
        rename_move_dentry(old_d, new_parent, new_name);
    }

out:
    if (new_d)
    {
        dentry_put(new_d);
    }
    dentry_put(old_d);
    if (new_parent)
    {
        dentry_put(new_parent);
    }
    return ret;
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
    return ENO16_PERM;
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
    return ENO16_PERM;
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
    pcb_t *cur_proc = proc_get_current();
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
 * @retval ENO0_NO_ERROR       成功，buf 填入以 '/' 开头的绝对路径
 * @retval ENO8_NULL_POINTER   参数为 NULL 或 size 为 0
 * @retval ENO11_NAME_TOO_LONG 路径超过 VFS_PATH_MAX，或缓冲区太小
 * @retval ENO5_NOSUCH_ENTRY   当前目录已被删除
 * @details 从 proc_cwd 沿 d_parent 向上，倒着写进缓冲区尾部；遇到文件系统局部根就跳到
 *   宿主文件系统里的挂载点目录项继续，所以挂载点下的 cwd 也能拼出完整路径。
 *   cwd 的深度与总长不受 VFS_PATH_MAX 约束（相对路径可以一层层 chdir 进去），
 *   所以每一步都要检查边界。
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

    pcb_t *cur_proc = proc_get_current();
    dentry_t *d = (cur_proc && cur_proc->proc_cwd) ? cur_proc->proc_cwd : vfs_root_dentry;

    char tmp[VFS_PATH_MAX];
    int pos = VFS_PATH_MAX - 1;
    tmp[pos] = '\0';

    while (d != vfs_root_dentry)
    {
        if (d->d_parent == d)
        {
            /* 文件系统局部根：跳到宿主文件系统里的挂载点目录项。已被删除的目录
             * 同样 d_parent 指向自身，但它不是任何挂载的根 */
            vfsmount_t *mnt = NULL;
            irq_key_t vfs_fs_lock_key = spinlock_acquire(&vfs_fs_lock);
            if (d->d_inode && d->d_inode->i_sb)
            {
                mnt = find_mount_by_sb(d->d_inode->i_sb);
            }
            spinlock_release(&vfs_fs_lock, vfs_fs_lock_key);

            if (mnt == NULL || mnt->mnt_host_dentry == NULL || mnt->mnt_sb == NULL ||
                mnt->mnt_sb->s_root_inode == NULL || mnt->mnt_sb->s_root_inode->i_dentry != d)
            {
                return ENO5_NOSUCH_ENTRY;
            }
            d = mnt->mnt_host_dentry;
            continue;
        }

        int nlen = (int)strlen(d->d_name);
        if (pos < nlen + 1)
        {
            return ENO11_NAME_TOO_LONG;
        }
        pos -= nlen;
        memcpy(tmp + pos, d->d_name, nlen);
        tmp[--pos] = '/';
        d = d->d_parent;
    }

    if (pos == VFS_PATH_MAX - 1)
    {
        tmp[--pos] = '/';
    }

    int len = VFS_PATH_MAX - 1 - pos;
    if ((size_t)(len + 1) > size)
    {
        return ENO11_NAME_TOO_LONG;
    }
    memcpy(buf, tmp + pos, len + 1);
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

/**
 * @brief 获取已打开文件的状态信息（fstat 用）
 * @param[in]  file    已打开的文件对象
 * @param[out] statbuf 输出的状态信息
 * @retval ENO0_NO_ERROR     成功
 * @retval ENO8_NULL_POINTER 参数为 NULL，或 file 没有关联的 inode
 * @details 直接读 file->f_inode，不需要像 vfs_stat 那样重新解析路径——
 *   打开文件时已经持有 inode 的一份引用，不存在"路径已被删除/改名"的竞态。
 */
int vfs_fstat(file_t *file, stat_t *statbuf)
{
    if (!file || !statbuf)
    {
        return ENO8_NULL_POINTER;
    }

    inode_t *inode = file->f_inode;
    if (!inode)
    {
        return ENO8_NULL_POINTER;
    }

    statbuf->st_ino   = inode->i_ino;
    statbuf->st_mode  = inode->i_mode;
    statbuf->st_size  = inode->i_size;
    statbuf->st_nlink = 1;

    return ENO0_NO_ERROR;
}

/**
 * @brief 读取目录项（getdents64 的 VFS 层入口）
 * @param[in]  file 已打开的目录文件对象
 * @param[out] buf  接收紧凑排列的 struct linux_dirent64 记录
 * @param[in]  len  buf 容量（字节）
 * @retval >0 已填字节数
 * @retval 0  目录已读完（EOF）
 * @retval ENO9_NOT_DIR file 不是目录
 * @retval ENO6_INVAL_PARAM 底层不支持 readdir，或缓冲区连一条记录都放不下
 * @details 薄转发——真正的记录组装（`.`/`..` 合成、`d_reclen` 8 字节对齐、
 *   放不下时的 pending 暂存）由具体文件系统的 readdir 回调负责。
 */
int vfs_getdents(file_t *file, void *buf, size_t len)
{
    if (!file || !buf)
    {
        return ENO8_NULL_POINTER;
    }
    if (!file->f_inode || !S_ISDIR(file->f_inode->i_mode))
    {
        return ENO9_NOT_DIR;
    }
    if (!file->f_op || !file->f_op->readdir)
    {
        return ENO6_INVAL_PARAM;
    }

    return file->f_op->readdir(file, buf, len);
}
