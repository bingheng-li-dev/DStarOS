# 第三方组件清单

DStarOS 整体以 **GPL-3.0-or-later** 发布（全文见 [LICENSE](LICENSE)）。仓库中包含若干
第三方源码，各自保留原作者的版权与许可证。本文件逐一列出它们的出处、许可证和改动情况。

`LICENSES/` 目录存放本仓库涉及的每一份许可证全文，包括本项目自己使用的 GPL-3。

---

## 清单

### Canaan K210 BSP — Apache-2.0

| 文件 | 相对导入时的改动 |
|---|---|
| `src/lib/bsp/include/encoding.h` | 有：补了 4 个 `SSTATUS_FS_*` 状态位宏；`SSTATUS_PUM` 按特权规范更名为 `SSTATUS_SUM` |
| `src/lib/bsp/include/atomic.h` | 无 |

- 版权：Copyright 2018 Canaan Inc.
- 上游：Kendryte K210 standalone SDK
- 许可证全文：[LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt)

Apache-2.0 与 GPL-2 不兼容（其专利与赔偿条款相对 GPL-2 构成附加限制），但与 GPL-3
兼容。**这两个文件是本项目必须选择 GPL-3 而非 GPL-2 的直接原因。**

### tinyprintf — LGPL-2.1-or-later

| 文件 | 相对导入时的改动 |
|---|---|
| `src/lib/bsp/tinyprintf.c` | 无 |
| `src/lib/bsp/include/tinyprintf.h` | 有：`<sys/types.h>` 改为 `<stddef.h>`（freestanding 环境无前者）；注释掉了 `#define printf tfp_printf` 的 libc 覆盖开关 |

- 版权：Copyright (C) 2004 Kustaa Nyholm
- 许可证全文：[LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt)
- 内核唯一的格式化输出实现（`-nostdlib` 环境下没有 libc 的 `printf`）

原许可证带 "or later" 条款，据此并入 GPL-3 发布。

### Linux 内核数据结构 — GPL-2.0-or-later

| 文件 | 相对导入时的改动 |
|---|---|
| `src/lib/core/include/list.h` | 无 |
| `src/lib/core/include/rbtree.h` | 无 |
| `src/lib/core/include/rbtree_augmented.h` | 无 |
| `src/lib/core/rbtree.c` | 无 |

- `list.h` 版权：Copyright (c) 2000-2002 Anton Altaparmakov and others（Linux-NTFS 项目）
- 红黑树版权：(C) 1999 Andrea Arcangeli、(C) 2002 David Woodhouse、(C) 2012 Michel Lespinasse
- 上游：Linux 内核
- 红黑树被 CFS 调度器（`src/kernel/sched.c`）使用；`list.h` 在内核各处作为侵入式链表

四个文件的许可证声明均为 "version 2 of the License, or (at your option) any later
version"，据此并入 GPL-3 发布。

### FatFs R0.11 — BSD-1-Clause 类

| 文件 | 相对导入时的改动 |
|---|---|
| `src/lib/fatfs/ff.c` | 无 |
| `src/lib/fatfs/include/ff.h` | 无 |
| `src/lib/fatfs/include/diskio.h` | 无 |
| `src/lib/fatfs/include/integer.h` | 无 |
| `src/lib/fatfs/include/ffconf.h` | 有：配置项按本项目需要调整（该文件的用途就是被使用者修改） |

- 版权：Copyright (C) 2015, ChaN, all right reserved.
- 上游：<http://elm-chan.org/fsw/ff/>
- 许可条款见 `src/lib/fatfs/ff.c` 文件头，属宽松式，与 GPL-3 兼容

注意 `src/lib/fatfs/diskio.c` **不是**第三方代码：它已被完全重写为本项目块设备层
（`bdev` / `bio`）的转发层，与上游同名文件无关。

---

## 不在本仓库内的依赖

以下组件是构建或运行 DStarOS 所需，但**其源码与二进制均未包含在本仓库中**，因此不产生
再分发义务：

| 组件 | 说明 |
|---|---|
| RustSBI | 固件/引导层。`bootloader/` 下的二进制未入库 |
| BusyBox | 用户态工具链。仓库只含构建脚本 `tools/build_busybox.sh` 与配置 `configs/busybox_dstar.config`，不含其源码 |
| riscv64 交叉工具链 | 外部安装 |

BusyBox 以独立进程被 `exec` 执行，与内核之间不存在链接关系，属于 GPL 意义上的
聚合（mere aggregation）而非演绎作品。

---

## 许可证兼容性小结

| 许可证 | 与 GPL-2 | 与 GPL-3 |
|---|---|---|
| Apache-2.0 | ✗ | ✓ |
| LGPL-2.1-or-later | ✓ | ✓（经 "or later" 升级） |
| GPL-2.0-or-later | ✓ | ✓（经 "or later" 升级） |
| BSD-1-Clause 类 | ✓ | ✓ |

四者的唯一共同解是 GPL-3，故本项目采用 GPL-3.0-or-later。
