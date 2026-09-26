#!/bin/bash
# 一口气跑完全部 19 套 QEMU 回归：自动切 src/debug/debug.h 的开关、重编、调 scripts/regress.sh，
# 结束时把开关复位成交付形态并重建内核。
#
# 用法（容器内，仓库任意目录）：
#   bash scripts/regress_all.sh                # 全部 19 套
#   bash scripts/regress_all.sh mem sig        # 只跑指定几套
#   RUNS=3 bash scripts/regress_all.sh pipe    # 每套连跑 3 次（抓偶发）
#
# 开关切换与 tools/build_vf2_suites.sh 同一套路数，三条都是踩出来的：
#   1. 切之前先把所有测试开关清零，再只开目标那一个——**只设目标开关不够**，上一套残留的
#      开关会在 proc.c 的 #elif 链里抢先生效，跑的是另一套测试，而且不会有任何报错；
#   2. bb 必须同时把 DEBUG_BUSYBOX_INTERACTIVE 设成 0，否则交互式 ash 停在提示符等到超时，
#      被报成 "suite did not finish"——看起来像回归失败，其实是驱动脚本漏了开关；
#   3. 结束时（含中途 Ctrl-C）一律复位，否则临时开关会被下一次提交带进去。
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

D=src/debug/debug.h
RUNS="${RUNS:-1}"
ALL_SUITES="sched slab dcache vfs file pipe tty mem exec sig time seg wait trap musl msys mroot mfp bb"
SUITES="${*:-$ALL_SUITES}"

switch_of()
{
    case "$1" in
        sched)  echo DEBUG_SCHED_TEST ;;     slab)  echo DEBUG_SLAB_TEST ;;
        dcache) echo DEBUG_DCACHE_TEST ;;    vfs)   echo DEBUG_VFS_TEST ;;
        file)   echo DEBUG_FILE_TEST ;;      pipe)  echo DEBUG_PIPE_TEST ;;
        tty)    echo DEBUG_TTY_TEST ;;       mem)   echo DEBUG_MEM_TEST ;;
        exec)   echo DEBUG_EXEC_TEST ;;      sig)   echo DEBUG_SIGNAL_TEST ;;
        time)   echo DEBUG_TIME_TEST ;;      seg)   echo DEBUG_SEG_TEST ;;
        wait)   echo DEBUG_WAIT_TEST ;;      trap)  echo DEBUG_TRAP_TEST ;;
        musl)   echo DEBUG_MUSL_TEST ;;      msys)  echo DEBUG_MSYSCHECK_TEST ;;
        mroot)  echo DEBUG_MROOTFS_TEST ;;   mfp)   echo DEBUG_MFP_TEST ;;
        bb)     echo DEBUG_BUSYBOX_TEST ;;
        *)      echo "" ;;
    esac
}

for s in $SUITES; do
    [ -n "$(switch_of "$s")" ] || { echo "regress_all: 未知套件 '$s'（可选：$ALL_SUITES）" >&2; exit 2; }
done

ALL_SWITCHES=$(for s in $ALL_SUITES; do switch_of "$s"; done)

# 宿主机偶尔会短暂占住 debug.h（编辑器、杀软），容器里 sed -i 的 rename 随之 Permission denied。
set_switch()
{
    local t
    for t in 1 2 3 4 5; do
        sed -i "s/^\(#define $1 \)[0-9][0-9]*/\1$2/" "$D" 2>/dev/null
        grep -qE "^#define $1 $2\b" "$D" && return 0
        sleep 1
    done
    echo "regress_all: 无法把 $1 设为 $2" >&2
    return 1
}

restore_switches()
{
    local m ok=0
    for m in $ALL_SWITCHES; do
        set_switch "$m" 0 || ok=1
    done
    set_switch DEBUG_BUSYBOX_INTERACTIVE 1 || ok=1
    return $ok
}

finish()
{
    echo
    restore_switches || echo "regress_all: ⚠️ 开关复位失败，提交前务必检查 $D" >&2
    if make >/dev/null 2>&1; then
        echo "开关已复位为交付形态，内核已重建。"
    else
        echo "regress_all: ⚠️ 交付形态内核重建失败" >&2
    fi
}
trap finish EXIT

started_at=$(date +%s)
fail=0
failed_suites=""
passed=0

for s in $SUITES; do
    sw=$(switch_of "$s")
    restore_switches || { fail=1; failed_suites="$failed_suites $s(开关)"; continue; }
    set_switch "$sw" 1 || { fail=1; failed_suites="$failed_suites $s(开关)"; continue; }
    if [ "$s" = bb ]; then
        set_switch DEBUG_BUSYBOX_INTERACTIVE 0 || { fail=1; failed_suites="$failed_suites $s(开关)"; continue; }
    fi

    # 只核对"目标开关为 1"不够，必须确认**恰好一个**为 1。
    tests_on=$(for m in $ALL_SWITCHES; do grep -E "^#define $m 1\b" "$D"; done | wc -l)
    if [ "$tests_on" != 1 ]; then
        echo "[$s] FAIL 测试开关有 $tests_on 个为 1"
        fail=1
        failed_suites="$failed_suites $s(开关)"
        continue
    fi

    log="build/regress-$s.log"
    if ! make >"$log" 2>&1; then
        echo "[$s] FAIL 构建失败，日志见 $log"
        grep -E "\.[ch]:[0-9]+:|error:" "$log" | head -3
        fail=1
        failed_suites="$failed_suites $s(构建)"
        continue
    fi
    warnings=$(grep -cE "warning:" "$log")
    rm -f "$log"

    out=$(bash scripts/regress.sh "$s" "$RUNS" 2>&1)
    echo "$out" | sed "s/^/    /"
    if echo "$out" | grep -q "FAILED"; then
        fail=1
        failed_suites="$failed_suites $s"
    else
        passed=$((passed + 1))
        [ "$warnings" != 0 ] && echo "[$s] ⚠️ 构建有 $warnings 条告警"
    fi
done

elapsed=$(( $(date +%s) - started_at ))
echo
echo "=== 通过 $passed / $(echo $SUITES | wc -w) 套，用时 $((elapsed / 60)) 分 $((elapsed % 60)) 秒 ==="
[ "$fail" -ne 0 ] && echo "未通过：$failed_suites"
exit "$fail"
