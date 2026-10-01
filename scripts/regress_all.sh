#!/bin/bash
# 跑全部或指定的 QEMU 回归套件（容器内）：自动切换 DEBUG_SUITE 并重编，结束时（含 Ctrl-C）复位为 SUITE_NONE。
#   bash scripts/regress_all.sh                # 全部 19 套
#   bash scripts/regress_all.sh mem sig        # 只跑指定几套
#   RUNS=3 bash scripts/regress_all.sh pipe    # 每套连跑 3 次
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
        *) echo "regress_all: unknown suite '$s' (choose from: $ALL_SUITES)" >&2; exit 2 ;;
    esac
done

# 宿主机上的编辑器或杀软偶尔占住 debug.h，sed -i 会失败，所以重试
set_suite()
{
    local t
    for t in 1 2 3 4 5; do
        sed -i "s/^#define DEBUG_SUITE .*/#define DEBUG_SUITE $1/" "$D" 2>/dev/null
        grep -qE "^#define DEBUG_SUITE $1\$" "$D" && return 0
        sleep 1
    done
    echo "regress_all: cannot set DEBUG_SUITE to $1" >&2
    return 1
}

finish()
{
    echo
    set_suite SUITE_NONE || echo "regress_all: WARNING: failed to reset DEBUG_SUITE, check $D before committing" >&2
    if make >/dev/null 2>&1; then
        echo "DEBUG_SUITE reset to SUITE_NONE, kernel rebuilt"
    else
        echo "regress_all: WARNING: failed to rebuild the delivery kernel" >&2
    fi
}
trap finish EXIT

started_at=$(date +%s)
fail=0
failed_suites=""
passed=0

for s in $SUITES; do
    set_suite "SUITE_$(echo "$s" | tr a-z A-Z)" || { fail=1; failed_suites="$failed_suites $s(switch)"; continue; }

    log="build/regress-$s.log"
    if ! make >"$log" 2>&1; then
        echo "[$s] FAIL build failed, see $log"
        grep -E "\.[ch]:[0-9]+:|error:" "$log" | head -3
        fail=1
        failed_suites="$failed_suites $s(build)"
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
        [ "$warnings" != 0 ] && echo "[$s] WARNING: $warnings compiler warning(s)"
    fi
done

elapsed=$(( $(date +%s) - started_at ))
echo
echo "=== passed $passed / $(echo $SUITES | wc -w) suites in $((elapsed / 60))m $((elapsed % 60))s ==="
[ "$fail" -ne 0 ] && echo "failed:$failed_suites"
exit "$fail"
