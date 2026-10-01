#!/bin/bash
# 以 PLATFORM=VF2 全量重建并产出 U-Boot booti 可启动的 build/vf2-kernel.img（容器内）。
# Image 头由 startup.S 的 .image_header 提供，这里逐字段校验：头错了 U-Boot 往往不报错，
# 内核直接跑飞。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OUT="build/vf2-kernel.img"
BIN="build/kernel.bin"

# 与 startup.S 的 .image_header、linker_vf2.ld、memtype.h 的 KERNEL_START 保持一致
EXP_TEXT_OFFSET="0000000000200000"
EXP_VERSION="00000002"
EXP_MAGIC="0000005643534952"         # "RISCV\0\0\0"
EXP_MAGIC2="05435352"                # "RSC\x05"

# 校验失败时不能留下上一次的镜像，否则会误烧旧版
rm -f "$OUT"

echo "build_vf2_image: full rebuild with PLATFORM=VF2"
make clean >/dev/null 2>&1 || true
make PLATFORM=VF2 >/dev/null

[ -f "$BIN" ] || { echo "build_vf2_image: $BIN was not produced" >&2; exit 1; }

field() {   # field <偏移> <字节数>
    od -An -tx"$2" -N"$2" -j"$1" "$BIN" | tr -d ' \n'
}

echo "build_vf2_image: checking Image header"
fail=0
check() {   # check <字段名> <实际> <期望>
    if [ "$2" = "$3" ]; then
        printf '  [OK]   %-12s %s\n' "$1" "$2"
    else
        printf '  [FAIL] %-12s got %s, expected %s\n' "$1" "$2" "$3"
        fail=1
    fi
}

# code0 必须是 4 字节 JAL；被压缩成 c.j 会让后面所有字段错位
CODE0="$(field 0 4)"
if [ "${CODE0: -2}" = "6f" ]; then
    printf '  [OK]   %-12s %s\n' "code0" "$CODE0"
else
    printf '  [FAIL] %-12s %s is not a 4-byte JAL, check .option norvc\n' "code0" "$CODE0"
    fail=1
fi

check "text_offset" "$(field 8  8)" "$EXP_TEXT_OFFSET"
check "version"     "$(field 32 4)" "$EXP_VERSION"
check "magic"       "$(field 48 8)" "$EXP_MAGIC"
check "magic2"      "$(field 56 4)" "$EXP_MAGIC2"

# image_size 含 .bss，不能小于文件大小
IMG_SIZE_HEX="$(field 16 8)"
IMG_SIZE=$((16#$IMG_SIZE_HEX))
FILE_SIZE=$(stat -c %s "$BIN")
if [ "$IMG_SIZE" -ge "$FILE_SIZE" ]; then
    printf '  [OK]   %-12s %s (%d bytes, file %d bytes)\n' \
           "image_size" "$IMG_SIZE_HEX" "$IMG_SIZE" "$FILE_SIZE"
else
    printf '  [FAIL] %-12s %d is smaller than the file (%d), check ekernel/skernel\n' \
           "image_size" "$IMG_SIZE" "$FILE_SIZE"
    fail=1
fi

[ "$fail" -eq 0 ] || { echo "build_vf2_image: Image header check failed, do not flash" >&2; exit 1; }

cp "$BIN" "$OUT"
echo "build_vf2_image: done -> $OUT ($(stat -c %s "$OUT") bytes)"
echo
# 地址写死：U-Boot 的 fdt_addr_r/ramdisk_addr_r 落在 PMM 页帧池内，会被内核覆盖
echo "U-Boot (TFTP):"
echo "  setenv dstar 'tftpboot 0x40200000 vf2-kernel.img && tftpboot 0x47000000 rootfs.img && booti 0x40200000 - \${fdtcontroladdr}'"
echo "  saveenv"
echo "  run dstar"
