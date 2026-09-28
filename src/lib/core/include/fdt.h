/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _FDT_H_
#define _FDT_H_

#include <stdint.h>
#include <stdbool.h>

#include "memtype.h"

/*
 * 扁平化设备树（Flattened Device Tree）的最小只读读取器。
 *
 * 这不是 libfdt 的子集，API 与之不兼容。将来若真要引入 libfdt，本文件整体退役，
 * 不要试图让两者共存或互相兼容。
 *
 * 刻意不做的三件事（它们才是 FDT 解析真正的复杂度来源）：
 *   - `reg` 属性的 `#address-cells` / `#size-cells` 继承：只有 /soc 那种嵌套才需要，
 *     而 /memory 是根节点的直接子节点，cells 从根节点直接读一个普通属性就够；
 *   - `ranges` 总线地址翻译：读 /soc 下外设地址才需要，我们的外设地址一律硬编码；
 *   - 全树 `compatible` 匹配：那是通用内核为支持成千上万块板子才需要的。
 *
 * 使用纪律：DTB 只用于校验与可选增强，不作为唯一真相来源。所有值都必须有
 * 编译期常量兜底；读到了就比对/覆盖，读不到就静默退回常量。这样解析器出错的最坏
 * 后果是"少一条自检"，而不是"拿到垃圾值然后炸掉"。
 *
 * @note 必须在 MMU 开启之前调用：DTB 由固件放置，不在内核偏移映射范围内，
 *   MMU 开启后再去读就是一个没建过映射的地址。两个平台各差一头：
 *     QEMU+RustSBI 放在 0x8005c000，在 KERNEL_START 之下；
 *     VF2 用 U-Boot 的 fdtcontroladdr（0xfffc56a0），在 KERNEL_MAP_END 之上。
 *   正确用法是开机时解析一次、把需要的值抄进全局，此后再不碰 DTB。
 */

/**
 * @brief 校验并记下 DTB 各段位置
 * @param[in] dtb_pa 固件通过 a1 传进来的 DTB 物理地址（见 startup.S）
 * @retval true  DTB 有效，后续查询可用
 * @retval false magic/版本不对或地址为 0；此时所有查询函数都会返回失败
 */
bool fdt_init(phyAddr_t dtb_pa);

/**
 * @brief DTB 是否解析成功、后续查询是否可用
 * @details 单独暴露出来是为了让调用方能区分"没有 dtb"和"dtb 里没有这个属性"——
 *   两者的诊断价值完全不同，混成一条日志会让排查绕远路。
 */
bool fdt_is_available(void);

/**
 * @brief 按绝对路径查找节点
 * @param[in] path 形如 "/cpus"、"/chosen"、"/memory"；只支持绝对路径
 * @return 指向该节点属性区起点的不透明指针；未找到返回 NULL
 * @note 路径分量按 '@' 截断比较，于是 "/memory" 能匹配到 "memory@80000000"。
 */
const void *fdt_find_node(const char *path);

/**
 * @brief 取节点的第一个直接子节点
 * @param[in] node 来自 fdt_find_node 或 fdt_next_subnode
 * @return 子节点属性区起点；没有子节点返回 NULL
 */
const void *fdt_first_subnode(const void *node);

/**
 * @brief 取同一层的下一个兄弟节点
 * @param[in] subnode 当前子节点（来自 fdt_first_subnode / fdt_next_subnode）
 * @return 下一个兄弟的属性区起点；已是最后一个返回 NULL
 * @note 会整体跳过当前节点的子树——/cpus/cpu@N 下面还挂着 interrupt-controller，
 *   只找下一个 BEGIN_NODE 会掉进子节点里去。
 */
const void *fdt_next_subnode(const void *subnode);

/**
 * @brief 读取节点下的属性原始数据
 * @param[in]  node 来自 fdt_find_node
 * @param[in]  name 属性名
 * @param[out] len  属性字节数（可传 NULL）
 * @return 属性数据指针（大端序，未解码）；不存在返回 NULL
 */
const void *fdt_get_prop(const void *node, const char *name, uint32_t *len);

/* 以下三个是 fdt_get_prop 的解码壳，长度不符时返回 false / NULL。 */
bool fdt_prop_u32(const void *node, const char *name, uint32_t *out);
bool fdt_prop_u64(const void *node, const char *name, uint64_t *out);
const char *fdt_prop_str(const void *node, const char *name);

#endif /* _FDT_H_ */
