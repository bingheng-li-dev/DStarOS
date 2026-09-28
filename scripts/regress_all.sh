#!/bin/bash
# 一口气跑完全部 19 套 QEMU 回归：自动切 src/debug/debug.h 的 DEBUG_SUITE、重编、调 scripts/regress.sh，
# 结束时把选择器复位成交付形态并重建内核。
#
# 用法（容器内，仓库任意目录）：
#   bash scripts/regress_all.sh                # 全部 19 套
#   bash scripts/regress_all.sh mem sig        # 只跑指定几套
#   RUNS=3 bash scripts/regress_all.sh pipe    # 每套连跑 3 次（抓偶发）
#
# 结束时（含中途 Ctrl-C）一律复位，否则临时选择会被下一次提交带进去。
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR" || exit 1

D=src/debug/debug.h
RUNS="${RUNS:-1}"
ALL_SUITES="sched slab dcache vfs file pipe tty mem exec sig time seg wait trap musl msys mroot mfp bb"
SUITES="${*:-$ALL_SUITES}"

for s in $SUITES; do
    case " $ALL_SUITES " in
        *" $s "*) ;;
        *) echo "regress_all: 未知套件 '$s'（可选：$ALL_SUITES）" >&2; exit 2 ;;
    esac
done

# 宿主机偶尔会短暂占住 debug.h（编辑器、杀软），容器里 sed -i 的 rename 随之 Permission denied。
set_suite()
{
    local t
    for t in 1 2 3 4 5; do
        sed -i "s/^#define DEBUG_SUITE .*/#define DEBUG_SUITE $1/" "$D" 2>/dev/null
        grep -qE "^#define DEBUG_SUITE $1\$" "$D" && return 0
        sleep 1
    done
    echo "regress_all: 无法把 DEBUG_SUITE 设为 $1" >&2
    return 1
}

finish()
{
    echo
    set_suite SUITE_NONE || echo "regress_all: ⚠️ DEBUG_SUITE 复位失败，提交前务必检查 $D" >&2
    if make >/dev/null 2>&1; then
        echo "DEBUG_SUITE 已复位为交付形态，内核已重建。"
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
    set_suite "SUITE_$(echo "$s" | tr a-z A-Z)" || { fail=1; failed_suites="$failed_suites $s(开关)"; continue; }

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
