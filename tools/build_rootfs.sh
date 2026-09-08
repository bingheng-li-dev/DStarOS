#!/bin/bash
# 造一个能被内核里的 FatFS 直接挂载的 FAT 根文件系统镜像。
#
# 用法（容器内，仓库任意目录）：
#   bash tools/build_rootfs.sh            # 产出 build/rootfs.img
#   ROOTFS_SIZE_KB=8192 bash tools/build_rootfs.sh
#
# 为什么用 mtools 而不是 loop mount：mcopy/mmd 直接读写镜像文件里的 FAT 结构，
# **不需要任何特权**；loop mount 要 CAP_SYS_ADMIN，容器里未必给。
#
# 镜像最终由 QEMU 的 -device loader 原样写进 rootfs 预留区（见 memtype.h 的
# ROOTFS_PHYS_BASE），内核侧看到的就是一块内存，不经过任何块设备驱动。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOTFS_IMG:-$ROOT_DIR/build/rootfs.img}"

# 镜像大小。只有一条硬约束：**必须 <= memtype.h 的 ROOTFS_MAX_SIZE（16 MB）**，
# 否则 -device loader 会写出预留区。
# 不需要等于 diskio.c 报给 FatFS 的扇区数——那个数是整个预留区的大小，只被 f_mkfs
# 用来决定格式化多大；f_mount 读的是镜像自己引导扇区里记的总扇区数。
# 于是镜像可以比预留区小，剩下的空间留着以后放大（BusyBox 进来时会用上）。
ROOTFS_SIZE_KB="${ROOTFS_SIZE_KB:-4096}"

# 扇区大小写死 512：ffconf.h 里 _MAX_SS 就是 512，FatFS 认不了别的。
SECTOR_SIZE=512

# 预留区上限，与 memtype.h 的 ROOTFS_MAX_SIZE 对齐（16 MB）。超了 -device loader
# 会把内容写到 PMM 的页帧池里去，那是**静默**的内存损坏，必须在这里挡住。
ROOTFS_MAX_KB=16384
if [ "$ROOTFS_SIZE_KB" -gt "$ROOTFS_MAX_KB" ]; then
    echo "build_rootfs: ${ROOTFS_SIZE_KB}KB 超过预留区上限 ${ROOTFS_MAX_KB}KB" >&2
    echo "  要放大得同时改 memtype.h 的 ROOTFS_MAX_SIZE 与 scripts/run.sh 的 -m" >&2
    exit 1
fi

need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "build_rootfs: 缺少 $1（v4 镜像里由 $2 提供）" >&2
        exit 1
    }
}
need mkfs.vfat dosfstools
need mmd mtools
need mcopy mtools
need mdir mtools

# mtools 默认会校验镜像的几何信息（磁头/柱面），裸文件系统镜像没有这些，
# 不关掉的话 mcopy 会拒绝工作。
export MTOOLS_SKIP_CHECK=1

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"

echo "build_rootfs: 造 ${ROOTFS_SIZE_KB} KB 空镜像 -> $OUT"
dd if=/dev/zero of="$OUT" bs=1024 count="$ROOTFS_SIZE_KB" status=none

# -F 16：显式指定 FAT16，不让 mkfs 按体积自己挑（挑出来的类型会随镜像大小变，
#        而内核侧的行为最好是确定的）。
# -s 1 ：1 扇区/簇。**4 MB 下这个不能省**——FAT16 要求至少 4085 个簇，
#        默认的 4 扇区/簇只能分出 2048 个，mkfs 会直接报
#        "Attempting to create a too small or a too large filesystem"。
#        镜像放大到 16 MB 以上时可以去掉 -s 1。
# -S 512：见上面 _MAX_SS。
# -n：卷标，纯粹为了 mdir 输出好认。
mkfs.vfat -F 16 -s 1 -S "$SECTOR_SIZE" -n DSTAROS "$OUT" >/dev/null

# 目录树。
# **刻意不建 /dev**：内核 fs_init() 挂完根文件系统之后自己 vfs_mkdir("/dev") 再挂 devfs，
# 而那句 mkdir 一旦返回非 0 就直接 return、devfs 挂不上（见 src/kernel/fs.c）。
# 镜像里预先建好 /dev 会让它撞 EEXIST，整个 /dev 就没了。
for d in bin sbin etc tmp; do
    mmd -i "$OUT" "::/$d"
done

# 用户程序统一放 /bin。fork_wait.elf 与 msyscheck.elf 的主名超过 8 字符，
# 会生成长文件名目录项——ffconf.h 里 _USE_LFN=3 打开了 LFN，vfs 回归也覆盖过这条路径。
shopt -s nullglob
elves=("$ROOT_DIR"/user/*.elf)
if [ ${#elves[@]} -eq 0 ]; then
    echo "build_rootfs: user/ 下没有 .elf，先在 user/ 里 make" >&2
    exit 1
fi
mcopy -i "$OUT" "${elves[@]}" ::/bin/
echo "build_rootfs: 拷入 ${#elves[@]} 个用户程序"

# BusyBox（tools/build_busybox.sh 的产物）。**不存在时不报错**：纯内核回归
# （sched/slab/vfs/dcache）不需要它，照"镜像不存在时内核仍能启动"的同一条思路。
# 只放一个文件、不造任何链接——FAT 没有符号链接，applet 分发靠 BusyBox 自己的
# CONFIG_FEATURE_SH_STANDALONE（见 configs/busybox_dstar.config）。
BUSYBOX_BIN="${BUSYBOX_BIN:-$ROOT_DIR/build/busybox}"
if [ -f "$BUSYBOX_BIN" ]; then
    mcopy -i "$OUT" "$BUSYBOX_BIN" ::/bin/busybox
    echo "build_rootfs: 拷入 busybox（$(stat -c %s "$BUSYBOX_BIN") 字节）"
else
    echo "build_rootfs: 未找到 $BUSYBOX_BIN，本次不放 busybox（需要时先跑 tools/build_busybox.sh）"
fi

# 放一个可读的小文件，给"内核能不能读到镜像里的内容"留一个不依赖 ELF 的判据。
printf 'DStarOS rootfs\n' > /tmp/.rootfs_stamp
mcopy -i "$OUT" /tmp/.rootfs_stamp ::/etc/issue
rm -f /tmp/.rootfs_stamp

echo "build_rootfs: 完成，$(stat -c %s "$OUT") 字节"
echo "--- ::/ ---"
mdir -i "$OUT" ::/
echo "--- ::/bin ---"
mdir -i "$OUT" ::/bin
