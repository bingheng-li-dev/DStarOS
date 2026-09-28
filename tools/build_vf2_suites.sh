#!/bin/bash
# 为板上回归逐套打 VF2 镜像：build/vf2-<suite>.img。
#
# 用法（容器内，仓库任意目录）：
#   bash tools/build_vf2_suites.sh              # 全部 19 套
#   bash tools/build_vf2_suites.sh wait mfp     # 只打指定的几套
#
# 每套把 DEBUG_SUITE 设成对应的 SUITE_*、以 PLATFORM=VF2 全量重建，然后逐个核对：
#   1. 零告警；
#   2. 内核里有这一套的特征串（内核态套件用收尾标记，用户态套件用 USER_PROGRAM_PATH）；
#   3. 所有 vf2-<suite>.img 的 CRC 互不相同——构建是确定性的，两套 CRC 相同就是打成了同一个镜像。
# 结束时（含中途失败）把 DEBUG_SUITE 复位成交付形态，并重建 build/vf2-kernel.img。
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

D=src/debug/debug.h
ALL_SUITES="sched slab dcache vfs file pipe tty mem exec sig time seg wait trap musl msys mroot mfp bb"
SUITES="${*:-$ALL_SUITES}"

signature_of() {
    case $1 in
        sched) echo 'SCHED TESTS DONE' ;;   slab)  echo 'slabtest done' ;;
        dcache) echo 'dcachetest done' ;;   vfs)   echo 'VFS test done' ;;
        file)  echo '/bin/filetest.elf' ;;  pipe)  echo '/bin/pipetest.elf' ;;
        tty)   echo '/bin/ttytest.elf' ;;   mem)   echo '/bin/memtest.elf' ;;
        exec)  echo '/bin/exectest.elf' ;;  sig)   echo '/bin/sigtest.elf' ;;
        time)  echo '/bin/timetest.elf' ;;  seg)   echo '/bin/segtest.elf' ;;
        wait)  echo '/bin/waittest.elf' ;;  trap)  echo '/bin/trapkill.elf' ;;
        musl)  echo '/bin/mhello.elf' ;;    msys)  echo '/bin/msyscheck.elf' ;;
        mroot) echo '/bin/mrootfs.elf' ;;   mfp)   echo '/bin/mfptest.elf' ;;
        bb)    echo '/bin/busybox' ;;
    esac
}

for s in $SUITES; do
    case " $ALL_SUITES " in
        *" $s "*) ;;
        *) echo "build_vf2_suites: 未知套件 '$s'（可选：$ALL_SUITES）" >&2; exit 2 ;;
    esac
done

# 宿主机偶尔会短暂占住 debug.h（编辑器、杀软），容器里 sed -i 的 rename 随之 Permission denied。
# 失败时重试，并以文件里实际的值为准。
set_suite() {
    local t
    for t in 1 2 3 4 5 6 7 8 9 10; do
        sed -i "s/^#define DEBUG_SUITE .*/#define DEBUG_SUITE $1/" "$D" 2>/dev/null
        grep -qE "^#define DEBUG_SUITE $1\$" "$D" && return 0
        sleep 1
    done
    echo "build_vf2_suites: 无法把 DEBUG_SUITE 设为 $1" >&2
    return 1
}

crc_of() {
    python3 -c "import zlib,sys;print('%08x'%(zlib.crc32(open(sys.argv[1],'rb').read())&0xffffffff))" "$1"
}

finish() {
    set_suite SUITE_NONE || echo "build_vf2_suites: ⚠️ DEBUG_SUITE 复位失败，提交前务必检查 $D" >&2
    if make vf2img >/dev/null 2>&1; then
        echo "交付镜像已重建：build/vf2-kernel.img crc32=$(crc_of build/vf2-kernel.img)"
    else
        echo "build_vf2_suites: ⚠️ 交付镜像重建失败" >&2
    fi
}
trap finish EXIT

fail=0
for s in $SUITES; do
    out="build/vf2-$s.img"
    rm -f "$out"

    set_suite "SUITE_$(echo "$s" | tr a-z A-Z)" || { fail=1; continue; }

    log="build/vf2-$s.log"
    if ! make vf2img >"$log" 2>&1; then
        echo "[$s] FAIL 构建失败，日志见 $log"
        grep -E "\.[ch]:[0-9]+:" "$log" | head -3
        fail=1
        continue
    fi
    warnings=$(grep -cE "warning:|error:" "$log")
    hits=$(strings build/kernel.elf | grep -cF "$(signature_of "$s")")
    cp build/vf2-kernel.img "$out"
    rm -f "$log"

    status=OK
    if [ "$warnings" != 0 ] || [ "$hits" = 0 ]; then
        status=FAIL
        fail=1
    fi
    printf '[%s] %s warnings=%s signature=%s size=0x%x crc32=%s\n' \
           "$s" "$status" "$warnings" "$hits" "$(stat -c %s "$out")" "$(crc_of "$out")"
done

# 去重要连同上一轮留下的其余镜像一起比：只重打几套时，也不能和没重打的撞上
dups=$(for s in $ALL_SUITES; do [ -f "build/vf2-$s.img" ] && crc_of "build/vf2-$s.img"; done | sort | uniq -d)
if [ -n "$dups" ]; then
    echo "FAIL 以下 CRC 在多个 vf2-<suite>.img 之间重复：$dups"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "=== 全部镜像核对通过 ==="
else
    echo "=== 有镜像未通过核对，不要上板 ==="
fi
echo
echo "U-Boot 侧（内核走 TFTP，rootfs 读 SD 卡）："
echo "  setenv dstar_t 'tftpboot 0x40200000 vf2-\${suite}.img && fatload mmc 1:1 0x47000000 rootfs.img && booti 0x40200000 - \${fdtcontroladdr}'"
echo "  saveenv"
echo "  setenv suite mem"
echo "  run dstar_t"
exit "$fail"
