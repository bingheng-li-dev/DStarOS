/*
 * fdt.c —— 扁平化设备树最小只读读取器。边界与使用纪律见 fdt.h。
 *
 * DTB 的二进制布局：
 *   struct fdt_header（全部大端）
 *   内存保留区表
 *   结构块：一串 token，深度优先描述整棵树
 *   字符串块：所有属性名首尾相接，属性里存的是它在这块里的偏移
 *
 * 结构块的 token：
 *   BEGIN_NODE(1) + 节点名（NUL 结尾，补齐到 4 字节）
 *   PROP(3)       + len(u32) + nameoff(u32) + 数据（补齐到 4 字节）
 *   END_NODE(2) / NOP(4) / END(9)
 */

#include "fdt.h"
#include "stringops.h"

#define FDT_MAGIC        0xd00dfeedU
#define FDT_MIN_VERSION   17U      /* size_dt_struct 是 v17 才有的字段，遍历要用它 */
#define FDT_LAST_COMP_VER 17U      /* 与 libfdt 的 FDT_LAST_SUPPORTED_VERSION 一致 */

#define FDT_BEGIN_NODE   0x1U
#define FDT_END_NODE     0x2U
#define FDT_PROP         0x3U
#define FDT_NOP          0x4U
#define FDT_END          0x9U

static const uint8_t *fdt_struct;      /* 结构块起点 */
static const uint8_t *fdt_struct_end;
static const char *fdt_strings;        /* 字符串块起点 */
static bool fdt_valid;

/* 按字节取大端整数：DTB 里所有整数都是大端，而且不保证自然对齐。 */
static uint32_t be32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  | (uint32_t)b[3];
}

static uint32_t align4(uint32_t n)
{
    return (n + 3U) & ~3U;
}

static bool str_eq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b)
    {
        a++;
        b++;
    }
    return *a == *b;
}

/* 节点名与路径分量比较：节点名可能带 unit-address 后缀（memory@80000000），
 * 按 '@' 截断之后再比，于是 "/memory" 能匹配到 "memory@80000000"。 */
static bool node_name_eq(const char *node_name, const char *comp, uint32_t comp_len)
{
    uint32_t i;

    for (i = 0; i < comp_len; i++)
    {
        if (node_name[i] == '\0' || node_name[i] != comp[i])
        {
            return false;
        }
    }
    return node_name[i] == '\0' || node_name[i] == '@';
}

bool fdt_init(phyAddr_t dtb_pa)
{
    fdt_valid = false;

    if (dtb_pa == 0)
    {
        return false;
    }

    /* MMU 尚未开启，物理地址可直接当指针用；开启之后这块地址不在内核映射里，
     * 所以本函数只能在 os_init_before_mmu_enable 阶段调用（见 fdt.h 的 @note）。 */
    const uint8_t *base = (const uint8_t *)dtb_pa;

    if (be32(base) != FDT_MAGIC)
    {
        return false;
    }

    /* 头部字段偏移：0 magic / 4 totalsize / 8 off_struct / 12 off_strings /
     * 16 off_rsvmap / 20 version / 24 last_comp_version / 28 boot_cpuid /
     * 32 size_strings / 36 size_struct。
     *
     * 版本判据踩过两次坑，记下来：
     *  - 要看的是 **version >= 17**，因为 size_dt_struct（偏移 36）是 v17 才加的字段，
     *    而下面的遍历要用它定边界。拿 version 跟 16 比会把每一棵现代 DTB 都拒掉。
     *  - last_comp_version 的上限是 **17 不是 16**（libfdt 的 FDT_LAST_SUPPORTED_VERSION
     *    就是 17）。实测 QEMU+RustSBI 传下来的树 version=17、last_comp_version=17，
     *    按 16 卡会直接判成"没有可用的 dtb"。 */
    if (be32(base + 20) < FDT_MIN_VERSION || be32(base + 24) > FDT_LAST_COMP_VER)
    {
        return false;
    }

    uint32_t off_struct  = be32(base + 8);
    uint32_t off_strings = be32(base + 12);
    uint32_t size_struct = be32(base + 36);

    fdt_struct     = base + off_struct;
    fdt_struct_end = fdt_struct + size_struct;
    fdt_strings    = (const char *)(base + off_strings);
    fdt_valid      = true;
    return true;
}

const void *fdt_find_node(const char *path)
{
    if (!fdt_valid || path == NULL || path[0] != '/')
    {
        return NULL;
    }

    /* 只匹配路径里的分量，根节点（名字为空、depth 1）不参与匹配。
     * matched 是已匹配的分量数；下一个待匹配分量必须出现在 depth == matched + 2 上。 */
    const uint8_t *p = fdt_struct;
    int depth = 0;
    int matched = 0;
    const char *comp = path + 1;              /* 指向当前待匹配分量 */

    /* 空路径 "/" 视为根节点，没有分量可匹配，直接不支持——调用方没有这种需求。 */
    if (*comp == '\0')
    {
        return NULL;
    }

    while (p + 4 <= fdt_struct_end)
    {
        uint32_t tok = be32(p);
        p += 4;

        if (tok == FDT_BEGIN_NODE)
        {
            const char *name = (const char *)p;
            p += align4((uint32_t)strlen(name) + 1U);
            depth += 1;

            if (depth == matched + 2)
            {
                /* 量出当前分量的长度（到 '/' 或结尾为止） */
                uint32_t len = 0;
                while (comp[len] != '\0' && comp[len] != '/')
                {
                    len += 1;
                }
                if (len > 0 && node_name_eq(name, comp, len))
                {
                    matched += 1;
                    if (comp[len] == '\0')
                    {
                        return p;   /* 全部分量匹配完，p 正指向该节点的属性区 */
                    }
                    comp += len + 1;
                }
            }
        }
        else if (tok == FDT_END_NODE)
        {
            /* 离开的若正是已匹配路径上的那个节点，要把匹配回退一格 */
            if (depth == matched + 1 && matched > 0)
            {
                matched -= 1;
                /* comp 回退到上一个分量：从 path 重新数 matched 个分量 */
                comp = path + 1;
                for (int i = 0; i < matched; i++)
                {
                    while (*comp != '\0' && *comp != '/')
                    {
                        comp++;
                    }
                    if (*comp == '/')
                    {
                        comp++;
                    }
                }
            }
            depth -= 1;
        }
        else if (tok == FDT_PROP)
        {
            uint32_t len = be32(p);
            p += 8;                 /* 跳过 len 与 nameoff */
            p += align4(len);
        }
        else if (tok == FDT_NOP)
        {
            continue;
        }
        else    /* FDT_END 或非法 token */
        {
            break;
        }
    }
    return NULL;
}

bool fdt_is_available(void)
{
    return fdt_valid;
}

const void *fdt_first_subnode(const void *node)
{
    if (!fdt_valid || node == NULL)
    {
        return NULL;
    }

    const uint8_t *p = (const uint8_t *)node;

    /* 规范要求属性全部排在子节点之前，所以跳完连续的 PROP 就到第一个子节点了。 */
    while (p + 4 <= fdt_struct_end)
    {
        uint32_t tok = be32(p);
        p += 4;

        if (tok == FDT_PROP)
        {
            uint32_t plen = be32(p);
            p += 8;
            p += align4(plen);
        }
        else if (tok == FDT_NOP)
        {
            continue;
        }
        else if (tok == FDT_BEGIN_NODE)
        {
            p += align4((uint32_t)strlen((const char *)p) + 1U);
            return p;
        }
        else    /* END_NODE：本节点没有子节点 */
        {
            break;
        }
    }
    return NULL;
}

const void *fdt_next_subnode(const void *subnode)
{
    if (!fdt_valid || subnode == NULL)
    {
        return NULL;
    }

    const uint8_t *p = (const uint8_t *)subnode;
    /* 传进来的指针已经在 subnode 内部，相对父节点的深度是 1。
     * 深度回到 0 表示刚离开 subnode（此时下一个 BEGIN_NODE 就是兄弟），
     * 变成 -1 表示连父节点都离开了，兄弟已经列完。 */
    int depth = 1;

    while (p + 4 <= fdt_struct_end)
    {
        uint32_t tok = be32(p);
        p += 4;

        if (tok == FDT_BEGIN_NODE)
        {
            p += align4((uint32_t)strlen((const char *)p) + 1U);
            if (depth == 0)
            {
                return p;
            }
            depth += 1;
        }
        else if (tok == FDT_END_NODE)
        {
            depth -= 1;
            if (depth < 0)
            {
                break;
            }
        }
        else if (tok == FDT_PROP)
        {
            uint32_t plen = be32(p);
            p += 8;
            p += align4(plen);
        }
        else if (tok == FDT_NOP)
        {
            continue;
        }
        else
        {
            break;
        }
    }
    return NULL;
}

const void *fdt_get_prop(const void *node, const char *name, uint32_t *len)
{
    if (!fdt_valid || node == NULL)
    {
        return NULL;
    }

    const uint8_t *p = (const uint8_t *)node;

    while (p + 4 <= fdt_struct_end)
    {
        uint32_t tok = be32(p);
        p += 4;

        if (tok == FDT_PROP)
        {
            uint32_t plen = be32(p);
            uint32_t noff = be32(p + 4);
            p += 8;
            if (str_eq(fdt_strings + noff, name))
            {
                if (len != NULL)
                {
                    *len = plen;
                }
                return p;
            }
            p += align4(plen);
        }
        else if (tok == FDT_NOP)
        {
            continue;
        }
        else
        {
            /* 规范要求属性排在子节点之前，所以撞见 BEGIN_NODE / END_NODE / END
             * 就说明本节点的属性已经列完了。 */
            break;
        }
    }
    return NULL;
}

bool fdt_prop_u32(const void *node, const char *name, uint32_t *out)
{
    uint32_t len = 0;
    const void *d = fdt_get_prop(node, name, &len);

    if (d == NULL || len != 4 || out == NULL)
    {
        return false;
    }
    *out = be32(d);
    return true;
}

bool fdt_prop_u64(const void *node, const char *name, uint64_t *out)
{
    uint32_t len = 0;
    const void *d = fdt_get_prop(node, name, &len);

    if (d == NULL || out == NULL)
    {
        return false;
    }
    /* 有些属性按 cells 写成 1 个或 2 个 u32，两种都认 */
    if (len == 4)
    {
        *out = be32(d);
        return true;
    }
    if (len == 8)
    {
        *out = ((uint64_t)be32(d) << 32) | be32((const uint8_t *)d + 4);
        return true;
    }
    return false;
}

const char *fdt_prop_str(const void *node, const char *name)
{
    uint32_t len = 0;
    const void *d = fdt_get_prop(node, name, &len);

    if (d == NULL || len == 0)
    {
        return NULL;
    }
    return (const char *)d;
}
