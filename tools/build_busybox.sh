#!/bin/bash
# 交叉编译 BusyBox，产出 build/busybox 供 tools/build_rootfs.sh 拷进根文件系统镜像。
#
# 用法（容器内，仓库任意目录）：
#   bash tools/build_busybox.sh              # 按 configs/busybox_dstar.config 编
#   bash tools/build_busybox.sh defconfig    # 用上游 defconfig 编（只验证构建链路）
#   bash tools/build_busybox.sh menuconfig   # 改配置，退出时把增量存回 configs/
#   BUSYBOX_SRC=/path/to/busybox bash tools/build_busybox.sh
#
# 源码不存在时自动获取：先下 busybox.net 的 tarball 并校验 sha256，失败再浅克隆 GitHub 镜像。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.38.0}"
BUSYBOX_SHA256="${BUSYBOX_SHA256:-34f9ea6ff8636f2c9241153b9114eefa9e65674a45318ae1ef95bb5f31c53bb2}"
BUSYBOX_TAG="${BUSYBOX_TAG:-${BUSYBOX_VERSION//./_}}"
BUSYBOX_COMMIT="${BUSYBOX_COMMIT:-fc71374df}"
BUSYBOX_URL="${BUSYBOX_URL:-https://busybox.net/downloads/busybox-$BUSYBOX_VERSION.tar.bz2}"
BUSYBOX_GIT="${BUSYBOX_GIT:-https://github.com/vda-linux/busybox_mirror.git}"
FETCH_TIMEOUT="${FETCH_TIMEOUT:-120}"

# 放在挂载卷上，重建容器不会丢
BUSYBOX_SRC="${BUSYBOX_SRC:-$ROOT_DIR/build/busybox-src}"
BUSYBOX_BUILD="${BUSYBOX_BUILD:-$ROOT_DIR/build/busybox-build}"

CROSS_COMPILE="${CROSS_COMPILE:-/root/riscv/toolchain-musl/bin/riscv64-linux-}"

CONFIG_FILE="$ROOT_DIR/configs/busybox_dstar.config"
OUT="$ROOT_DIR/build/busybox"

MODE="${1:-config}"

die() {
    echo "build_busybox: $*" >&2
    exit 1
}

need() {
    command -v "$1" >/dev/null 2>&1 || die "missing $1 (provided by $2)"
}
need make make
need gcc build-essential
need bzip2 bzip2
[ -x "${CROSS_COMPILE}gcc" ] || die "cross compiler not found: ${CROSS_COMPILE}gcc"

fetch_tarball() {
    local tmp="$1" tarball="$1/busybox.tar.bz2"
    echo "build_busybox: fetching $BUSYBOX_URL"
    timeout "$FETCH_TIMEOUT" wget -q --tries=2 --timeout=15 -O "$tarball" "$BUSYBOX_URL" || return 1
    if ! echo "$BUSYBOX_SHA256  $tarball" | sha256sum -c --status; then
        echo "build_busybox: sha256 mismatch for $BUSYBOX_URL" >&2
        return 1
    fi
    tar -xjf "$tarball" -C "$tmp" --strip-components=1 && rm -f "$tarball"
}

fetch_git() {
    local tmp="$1"
    echo "build_busybox: cloning $BUSYBOX_GIT ($BUSYBOX_TAG)"
    rm -rf "$tmp"
    GIT_TERMINAL_PROMPT=0 timeout "$FETCH_TIMEOUT" \
        git clone -q --depth 1 --branch "$BUSYBOX_TAG" "$BUSYBOX_GIT" "$tmp"
}

fetch_source() {
    local tmp="$BUSYBOX_SRC.tmp"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    if fetch_tarball "$tmp" || fetch_git "$tmp"; then
        echo "$BUSYBOX_VERSION" > "$tmp/.dstar-version"
        mv "$tmp" "$BUSYBOX_SRC"
        return 0
    fi
    rm -rf "$tmp"
    die "cannot fetch BusyBox $BUSYBOX_VERSION; download it manually, e.g.
  git clone --depth 1 --branch $BUSYBOX_TAG $BUSYBOX_GIT $BUSYBOX_SRC"
}

check_source_version() {
    if [ -d "$BUSYBOX_SRC/.git" ]; then
        local have
        have=$(cd "$BUSYBOX_SRC" && git rev-parse --short HEAD)
        case "$BUSYBOX_COMMIT" in
            "$have"*|"") ;;
            *) echo "build_busybox: warning: source at $have, expected $BUSYBOX_COMMIT ($BUSYBOX_TAG)" >&2 ;;
        esac
        if [ -n "$(cd "$BUSYBOX_SRC" && git status --porcelain)" ]; then
            echo "build_busybox: warning: source tree has local changes, build is not reproducible" >&2
        fi
    elif [ -f "$BUSYBOX_SRC/.dstar-version" ]; then
        local have
        have=$(cat "$BUSYBOX_SRC/.dstar-version")
        [ "$have" = "$BUSYBOX_VERSION" ] ||
            echo "build_busybox: warning: source is $have, expected $BUSYBOX_VERSION" >&2
    fi
}

[ -d "$BUSYBOX_SRC" ] || fetch_source
check_source_version

mkdir -p "$BUSYBOX_BUILD" "$(dirname "$OUT")"

mk() {
    make -C "$BUSYBOX_SRC" O="$BUSYBOX_BUILD" ARCH=riscv CROSS_COMPILE="$CROSS_COMPILE" "$@"
}

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

verify_must_be_off() {
    local cfg="$BUSYBOX_BUILD/.config" sym rc=0
    for sym in CONFIG_ASH_JOB_CONTROL CONFIG_FEATURE_EDITING; do
        if grep -q "^${sym}=y" "$cfg"; then
            echo "build_busybox: $sym is enabled but not supported by this kernel" >&2
            rc=1
        fi
    done
    return $rc
}

resolve_config() {
    yes "" | mk oldconfig >/dev/null 2>&1 || true
}

case "$MODE" in
    defconfig)
        # 上游 defconfig 是动态链接的，产物跑不进本内核，只证明构建链路通
        echo "build_busybox: using upstream defconfig (toolchain check only, not runnable on this kernel)"
        mk defconfig >/dev/null
        ;;
    menuconfig)
        [ -f "$CONFIG_FILE" ] || die "missing $CONFIG_FILE"
        mk allnoconfig >/dev/null 2>&1
        apply_fragment "$CONFIG_FILE"
        resolve_config
        mk menuconfig
        cp "$BUSYBOX_BUILD/.config" "$BUSYBOX_BUILD/.config.tuned"
        mk allnoconfig >/dev/null 2>&1
        cp "$BUSYBOX_BUILD/.config" "$BUSYBOX_BUILD/.config.base"
        {
            echo "# Delta against allnoconfig, exported by tools/build_busybox.sh menuconfig."
            echo
            grep -E "^(CONFIG_|# CONFIG_)" "$BUSYBOX_BUILD/.config.tuned" |
                grep -vxFf <(grep -E "^(CONFIG_|# CONFIG_)" "$BUSYBOX_BUILD/.config.base")
        } > "$CONFIG_FILE"
        cp "$BUSYBOX_BUILD/.config.tuned" "$BUSYBOX_BUILD/.config"
        echo "build_busybox: delta saved to $CONFIG_FILE"
        ;;
    config)
        [ -f "$CONFIG_FILE" ] || die "missing $CONFIG_FILE (run 'menuconfig' to create one, or 'defconfig' to check the toolchain)"
        # 从 allnoconfig 起步：defconfig 开着约 390 个 applet，关漏一个不会有任何提示；
        # 从全关起步，片段里写的就是全部
        mk allnoconfig >/dev/null 2>&1
        apply_fragment "$CONFIG_FILE"
        resolve_config
        verify_must_be_off || exit 1
        ;;
    *)
        die "unknown mode '$MODE' (config | defconfig | menuconfig)"
        ;;
esac

mk -j"$(nproc)"

cp "$BUSYBOX_BUILD/busybox" "$OUT"
echo "build_busybox: done -> $OUT ($(stat -c %s "$OUT") bytes)"
file "$OUT" 2>/dev/null || true
