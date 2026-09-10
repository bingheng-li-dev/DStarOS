#!/bin/bash
# 产出可被 U-Boot `booti` 直接启动的 VF2 内核镜像。
#
# 用法（容器内，仓库任意目录）：
#   bash tools/build_vf2_image.sh
#
# 做的事其实很少——Image 头由 startup.S 的 .image_header 段提供，链接脚本
# lds/linker_vf2.ld 把它放在镜像第 0 字节，makefile 的 objcopy 已经产出
# build/kernel.bin。本脚本的真正价值是**逐字段校验那 64 字节头**：
# 头错了 U-Boot 只会报一句 "Bad Linux RISCV Image magic!"，
# 而更坏的情况是 magic 恰好对、text_offset 错，那会把内核搬到错误的地址、
# 一执行就跑飞，且没有任何提示。烧录之前多花一秒钟校验，比在板子上二分便宜得多。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OUT="build/vf2-kernel.img"
BIN="build/kernel.bin"

# 期望值。**改任何一个都要同步改三处**：本文件、startup.S 的 .image_header、
# lds/linker_vf2.ld 的 PHYS_BASE_ADDRESS / memtype.h 的 KERNEL_START。
EXP_TEXT_OFFSET="0000000000200000"   # 2 MB，与 KERNEL_START 的低位一致
EXP_VERSION="00000002"               # major 0 / minor 2
EXP_MAGIC="0000005643534952"         # "RISCV\0\0\0"
EXP_MAGIC2="05435352"                # "RSC\x05"

# 先删旧产物：校验失败时会直接 exit，若不删，上一次成功的镜像会留在原地，
# 而它看起来和新的一模一样——"以为烧的是新版、其实是旧版"是最难查的一类错。
rm -f "$OUT"

echo "build_vf2_image: 以 PLATFORM=VF2 全量重建"
# 必须 clean：换平台时编译期宏变了但 .o 时间戳没变，make 会静默复用上一个平台的产物。
# （makefile 里已有平台戳自动处理，这里再来一次是因为本脚本可能被单独调用。）
make clean >/dev/null 2>&1 || true
make PLATFORM=VF2 >/dev/null

[ -f "$BIN" ] || { echo "build_vf2_image: 没有产出 $BIN" >&2; exit 1; }

field() {   # field <偏移> <字节数>
    od -An -tx"$2" -N"$2" -j"$1" "$BIN" | tr -d ' \n'
}

echo "build_vf2_image: 校验 Image 头"
fail=0
check() {   # check <字段名> <实际> <期望>
    if [ "$2" = "$3" ]; then
        printf '  [OK]   %-12s %s\n' "$1" "$2"
    else
        printf '  [FAIL] %-12s 实际 %s，期望 %s\n' "$1" "$2" "$3"
        fail=1
    fi
}

CODE0="$(field 0 4)"
# code0 必须是一条**4 字节**的跳转（opcode 0x6f = JAL）。开着 C 扩展时 `j` 会被
# 压缩成 2 字节的 c.j，把后面每个字段都错位——startup.S 里用 .option norvc 挡住了，
# 这里再验一次，因为它错了之后所有别的字段看起来也会是错的，容易误判成别的问题。
if [ "${CODE0: -2}" = "6f" ]; then
    printf '  [OK]   %-12s %s (JAL，4 字节未被压缩)\n' "code0" "$CODE0"
else
    printf '  [FAIL] %-12s %s —— 不是 4 字节 JAL，检查 .option norvc\n' "code0" "$CODE0"
    fail=1
fi

check "text_offset" "$(field 8  8)" "$EXP_TEXT_OFFSET"
check "version"     "$(field 32 4)" "$EXP_VERSION"
check "magic"       "$(field 48 8)" "$EXP_MAGIC"
check "magic2"      "$(field 56 4)" "$EXP_MAGIC2"

# image_size 没有固定期望值，但必须**不小于文件本身**——它含 .bss，
# 而 .bss 不占文件空间。反过来若它比文件还小，说明链接脚本里 ekernel/skernel 算错了。
IMG_SIZE_HEX="$(field 16 8)"
IMG_SIZE=$((16#$IMG_SIZE_HEX))
FILE_SIZE=$(stat -c %s "$BIN")
if [ "$IMG_SIZE" -ge "$FILE_SIZE" ]; then
    printf '  [OK]   %-12s %s (%d 字节，文件 %d 字节，差值是 .bss)\n' \
           "image_size" "$IMG_SIZE_HEX" "$IMG_SIZE" "$FILE_SIZE"
else
    printf '  [FAIL] %-12s %d 小于文件大小 %d —— ekernel/skernel 算错了\n' \
           "image_size" "$IMG_SIZE" "$FILE_SIZE"
    fail=1
fi

[ "$fail" -eq 0 ] || { echo "build_vf2_image: Image 头校验未通过，**不要烧录**" >&2; exit 1; }

cp "$BIN" "$OUT"
echo "build_vf2_image: 完成 -> $OUT（$(stat -c %s "$OUT") 字节）"
echo
echo "U-Boot 侧用法（走 TFTP，地址已按实机 printenv 逐条核对）："
echo "  tftpboot 0x40200000 vf2-kernel.img"
echo "  tftpboot 0x47000000 rootfs.img"
echo "  booti 0x40200000 - \${fdtcontroladdr}"
echo
echo "存成一条命令，之后每轮迭代只敲 run dstar："
echo "  setenv tftpwindowsize 8"
echo "  setenv dstar 'tftpboot 0x40200000 vf2-kernel.img; tftpboot 0x47000000 rootfs.img; booti 0x40200000 - \${fdtcontroladdr}'"
echo "  saveenv"
echo
echo "选 TFTP 而不是 SD 卡，是因为换卡要断电、拆卡、占用读卡器的 USB 口——"
echo "而那个口往往就是串口转接头在用的，等于每轮迭代都要拔掉唯一的观测手段。"
echo
echo "⚠️ 三个地址都是写死的，**不要**换成 U-Boot 的同名默认变量："
echo "   fdt_addr_r=0x46000000 与 ramdisk_addr_r=0x46100000 都落在 PMM 页帧池"
echo "   [0x40200000, 0x47000000) 内，会被当空闲页分配出去改坏，且没有任何报错。"
echo "   kernel_addr_r 恰好就是 0x40200000，用不用变量都一样。"
echo "   fdtcontroladdr=0xfffc56a0 是 U-Boot 自带的控制 DTB，在 KERNEL_MAP_END 之外，"
echo "   PMM 够不着，而且不依赖 SD 卡上有没有 dtb 文件。"
