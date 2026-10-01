#!/bin/bash
# 造 FAT16 根文件系统镜像 build/rootfs.img（容器内）：user/*.elf、/sbin/init、BusyBox。
#   bash tools/build_rootfs.sh
#   ROOTFS_SIZE_KB=8192 bash tools/build_rootfs.sh
# 用 mtools 直接写镜像，不需要 loop mount 的特权。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOTFS_IMG:-$ROOT_DIR/build/rootfs.img}"

ROOTFS_SIZE_KB="${ROOTFS_SIZE_KB:-4096}"
# ffconf.h 的 _MAX_SS
SECTOR_SIZE=512

# 必须不超过 memtype.h 的 ROOTFS_MAX_SIZE，否则 QEMU 装载时会写进 PMM 的页帧池
ROOTFS_MAX_KB=16384
if [ "$ROOTFS_SIZE_KB" -gt "$ROOTFS_MAX_KB" ]; then
    echo "build_rootfs: ${ROOTFS_SIZE_KB}KB exceeds the ${ROOTFS_MAX_KB}KB reserved region" >&2
    echo "  to grow it, change ROOTFS_MAX_SIZE in memtype.h and -m in scripts/run.sh" >&2
    exit 1
fi

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "build_rootfs: missing $1 (provided by $2)" >&2
        exit 1
    }
}
need mkfs.vfat dosfstools
need mmd mtools
need mcopy mtools
need mdir mtools

# 裸文件系统镜像没有磁头/柱面信息，不关掉校验 mtools 会拒绝工作
export MTOOLS_SKIP_CHECK=1

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"

echo "build_rootfs: creating ${ROOTFS_SIZE_KB} KB image -> $OUT"
dd if=/dev/zero of="$OUT" bs=1024 count="$ROOTFS_SIZE_KB" status=none

# 固定 FAT16；4 MB 下必须 -s 1，否则凑不够 FAT16 要求的 4085 个簇
mkfs.vfat -F 16 -s 1 -S "$SECTOR_SIZE" -n DSTAROS "$OUT" >/dev/null

# 不建 /dev：fs_init() 自己 mkdir /dev 再挂 devfs，目录已存在会导致 devfs 挂不上
for d in bin sbin etc tmp; do
    mmd -i "$OUT" "::/$d"
done

shopt -s nullglob
elves=()
for e in "$ROOT_DIR"/user/*.elf; do
    [ "$(basename "$e")" = "init.elf" ] && continue
    elves+=("$e")
done
if [ ${#elves[@]} -eq 0 ]; then
    echo "build_rootfs: no .elf in user/, run 'make -C user' first" >&2
    exit 1
fi
mcopy -i "$OUT" "${elves[@]}" ::/bin/
echo "build_rootfs: copied ${#elves[@]} user programs"

# init 与 busybox 缺失时不报错：内核态套件用不到它们
INIT_BIN="$ROOT_DIR/user/init.elf"
if [ -f "$INIT_BIN" ]; then
    mcopy -i "$OUT" "$INIT_BIN" ::/sbin/init
    echo "build_rootfs: copied /sbin/init ($(stat -c %s "$INIT_BIN") bytes)"
else
    echo "build_rootfs: $INIT_BIN not found, skipping /sbin/init"
fi

# 只放一个 busybox，不造 applet 链接（FAT 没有符号链接，分发靠 FEATURE_SH_STANDALONE）
BUSYBOX_BIN="${BUSYBOX_BIN:-$ROOT_DIR/build/busybox}"
if [ -f "$BUSYBOX_BIN" ]; then
    mcopy -i "$OUT" "$BUSYBOX_BIN" ::/bin/busybox
    echo "build_rootfs: copied busybox ($(stat -c %s "$BUSYBOX_BIN") bytes)"
else
    echo "build_rootfs: $BUSYBOX_BIN not found, skipping busybox (run tools/build_busybox.sh)"
fi

# mroot、wait、bb 套件会读这个文件
printf 'DStarOS rootfs\n' > /tmp/.rootfs_stamp
mcopy -i "$OUT" /tmp/.rootfs_stamp ::/etc/issue
rm -f /tmp/.rootfs_stamp

echo "build_rootfs: done, $(stat -c %s "$OUT") bytes"
echo "--- ::/ ---"
mdir -i "$OUT" ::/
echo "--- ::/bin ---"
mdir -i "$OUT" ::/bin
