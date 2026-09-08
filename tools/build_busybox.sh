#!/bin/bash
# 交叉编译 BusyBox，产出 build/busybox 供 tools/build_rootfs.sh 拷进根文件系统镜像。
#
# 用法（容器内，仓库任意目录）：
#   bash tools/build_busybox.sh              # 按 configs/busybox_dstar.config 编
#   bash tools/build_busybox.sh defconfig    # 用上游 defconfig 编（只为验证构建链路）
#   bash tools/build_busybox.sh menuconfig   # 改配置，退出时自动把增量存回 configs/
#   BUSYBOX_SRC=/path/to/busybox bash tools/build_busybox.sh
#
# 源码树**不进 git**（10 MB+，是外部项目），进 git 的是本脚本记的版本号与
# configs/busybox_dstar.config——那两样合起来就是"这个 busybox 是怎么来的"的完整记录。
# 这条原则与阶段 9 处理 musl 工具链的方式一致：工具链 103 MB 不进 git，
# 但 docker/Dockerfile 里的版本 + URL + sha256 进了。
#
# **out-of-tree 构建**（Kbuild 的 O=）：产物全部落在 BUSYBOX_BUILD，源码树保持干净。
# 这样源码树可以是一个只读的 git checkout，切 tag 不会撞上未跟踪的构建产物。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 源码树。busybox.net 实测不可达（30 s 超时），源码走 GitHub 镜像：
#   git clone https://github.com/mirror/busybox /root/riscv/busybox_mirror
#   cd /root/riscv/busybox_mirror && git checkout 1_38_0
BUSYBOX_SRC="${BUSYBOX_SRC:-/root/riscv/busybox_mirror}"
BUSYBOX_TAG="${BUSYBOX_TAG:-1_38_0}"
BUSYBOX_COMMIT="${BUSYBOX_COMMIT:-fc71374df}"   # 1_38_0 指向的提交，用于核对

# 构建目录刻意放在仓库之外：它有几千个 .o，放 build/ 下会让 git status 变得没法看，
# 也会被 make clean 之类的规则误伤。
BUSYBOX_BUILD="${BUSYBOX_BUILD:-/root/riscv/busybox-build}"

CROSS_COMPILE="${CROSS_COMPILE:-/root/riscv/toolchain-musl/bin/riscv64-linux-}"

CONFIG_FILE="$ROOT_DIR/configs/busybox_dstar.config"
OUT="$ROOT_DIR/build/busybox"

MODE="${1:-config}"

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "build_busybox: 缺少 $1（v4 镜像里由 $2 提供）" >&2
        exit 1
    }
}
need make make
need gcc build-essential          # BusyBox 要用**宿主** gcc 编它自己的构建工具
[ -x "${CROSS_COMPILE}gcc" ] || {
    echo "build_busybox: 找不到交叉编译器 ${CROSS_COMPILE}gcc" >&2
    exit 1
}
[ -d "$BUSYBOX_SRC" ] || {
    echo "build_busybox: 源码树不存在：$BUSYBOX_SRC" >&2
    echo "  git clone https://github.com/mirror/busybox $BUSYBOX_SRC" >&2
    echo "  cd $BUSYBOX_SRC && git checkout $BUSYBOX_TAG" >&2
    exit 1
}

# 版本核对只警告不拦：换版本试问题是正常操作，但**必须让它出现在日志里**，
# 否则"编出来的 busybox 到底是哪个版本"就只能靠记忆了。
if [ -d "$BUSYBOX_SRC/.git" ]; then
    have=$(cd "$BUSYBOX_SRC" && git rev-parse --short HEAD)
    case "$BUSYBOX_COMMIT" in
        "$have"*|"") ;;
        *) echo "build_busybox: 警告——源码在 $have，本脚本记录的是 $BUSYBOX_COMMIT ($BUSYBOX_TAG)" >&2 ;;
    esac
    if [ -n "$(cd "$BUSYBOX_SRC" && git status --porcelain)" ]; then
        echo "build_busybox: 警告——源码树有未提交改动（打过补丁？），产物不可复现" >&2
    fi
fi

mkdir -p "$BUSYBOX_BUILD" "$(dirname "$OUT")"

mk() {
    make -C "$BUSYBOX_SRC" O="$BUSYBOX_BUILD" ARCH=riscv CROSS_COMPILE="$CROSS_COMPILE" "$@"
}

# 把一份 kconfig 片段应用到 .config 上。
# 片段的格式就是标准的两种行：`CONFIG_X=值` 与 `# CONFIG_X is not set`，其余 # 开头的
# 行是注释。**先删后加**，这样重复应用是幂等的。
apply_fragment() {
    local frag="$1" cfg="$BUSYBOX_BUILD/.config" sym val
    while IFS= read -r line; do
        case "$line" in
            CONFIG_*=*)
                sym="${line%%=*}"; val="${line#*=}"
                sed -i "/^${sym}=/d; /^# ${sym} is not set\$/d" "$cfg"
                echo "${sym}=${val}" >> "$cfg"
                ;;
            "# CONFIG_"*" is not set")
                sym="${line#\# }"; sym="${sym%% is not set}"
                sed -i "/^${sym}=/d; /^# ${sym} is not set\$/d" "$cfg"
                echo "# ${sym} is not set" >> "$cfg"
                ;;
        esac
    done < "$frag"
}

# 应用完片段后复核那几条**必须关掉**的开关。
# oldconfig 会给新出现的符号填默认值，而 ASH_JOB_CONTROL 之类的默认是 y——
# 一旦哪天它没被关住，症状是运行时才暴露的（内核没有 stop/cont 语义），
# 那时排查成本高得多。**配置错了要在构建期就响。**
verify_must_be_off() {
    local cfg="$BUSYBOX_BUILD/.config" sym rc=0
    for sym in CONFIG_ASH_JOB_CONTROL CONFIG_FEATURE_EDITING; do
        if grep -q "^${sym}=y" "$cfg"; then
            echo "build_busybox: $sym 被打开了，本内核不支持（见 configs/busybox_dstar.config）" >&2
            rc=1
        fi
    done
    return $rc
}

# BusyBox 1.38 的 kconfig **没有 olddefconfig**，只有会逐项提问的 oldconfig；
# 喂空行即取默认值。所有已有符号（allnoconfig 写满了显式值 + 我们的片段）保持原样，
# 只有因为打开 ASH 等而新可见的子选项会取默认。
resolve_config() {
    yes "" | mk oldconfig >/dev/null 2>&1 || true
}

case "$MODE" in
    defconfig)
        # 上游 defconfig：什么都不裁，**只用来证明构建链路本身是通的**。
        # 它是动态链接的（CONFIG_STATIC=n），跑不进本内核，这是预期行为。
        echo "build_busybox: 使用上游 defconfig（仅验证构建链路，产物不可用于本内核）"
        mk defconfig >/dev/null
        ;;
    menuconfig)
        [ -f "$CONFIG_FILE" ] || { echo "build_busybox: 缺少 $CONFIG_FILE" >&2; exit 1; }
        mk allnoconfig >/dev/null 2>&1
        apply_fragment "$CONFIG_FILE"
        resolve_config
        mk menuconfig
        # 改完自动把**增量**存回 configs/：靠人记得手动导出的话，迟早出现
        # "编出来的 busybox 和仓库里记的配置对不上"。
        cp "$BUSYBOX_BUILD/.config" "$BUSYBOX_BUILD/.config.tuned"
        mk allnoconfig >/dev/null 2>&1
        cp "$BUSYBOX_BUILD/.config" "$BUSYBOX_BUILD/.config.base"
        {
            echo "# 由 tools/build_busybox.sh menuconfig 自动导出的增量（相对 allnoconfig）。"
            echo "# 手工编辑时可以加注释说明每条决策的理由——重新导出会覆盖掉，记得补回。"
            echo
            grep -E "^(CONFIG_|# CONFIG_)" "$BUSYBOX_BUILD/.config.tuned" |
                grep -vxFf <(grep -E "^(CONFIG_|# CONFIG_)" "$BUSYBOX_BUILD/.config.base")
        } > "$CONFIG_FILE"
        cp "$BUSYBOX_BUILD/.config.tuned" "$BUSYBOX_BUILD/.config"
        echo "build_busybox: 增量已存回 $CONFIG_FILE"
        ;;
    config)
        [ -f "$CONFIG_FILE" ] || {
            echo "build_busybox: 缺少 $CONFIG_FILE" >&2
            echo "  先跑 'bash tools/build_busybox.sh menuconfig' 裁一份出来，" >&2
            echo "  或 'bash tools/build_busybox.sh defconfig' 只验证构建链路。" >&2
            exit 1
        }
        # 从 allnoconfig 起步而不是 defconfig：defconfig 打开约 390 个 applet，
        # 要关的比要留的多一个数量级，而且**关漏一个是静默的**（多一个用不上的命令，
        # 没人会发现）。从全关起步，片段里写着的就是全部，漏掉什么一目了然。
        mk allnoconfig >/dev/null 2>&1
        apply_fragment "$CONFIG_FILE"
        resolve_config
        verify_must_be_off || exit 1
        ;;
    *)
        echo "build_busybox: 未知模式 '$MODE'（config | defconfig | menuconfig）" >&2
        exit 1
        ;;
esac

mk -j"$(nproc)"

cp "$BUSYBOX_BUILD/busybox" "$OUT"
echo "build_busybox: 完成 -> $OUT（$(stat -c %s "$OUT") 字节）"
file "$OUT" 2>/dev/null || true
