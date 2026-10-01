#!/bin/bash
# 跑单个回归套件（容器内）：bash scripts/regress.sh <suite> [runs]
#   suite: sched | slab | dcache | vfs | file | pipe | tty | mem | exec | sig | time | seg | wait | trap | musl | msys | mroot | mfp | bb
# 需先把 src/debug/debug.h 的 DEBUG_SUITE 改成对应的 SUITE_* 再 make；一次跑全部用 regress_all.sh。
cd "$(dirname "$0")/.." || exit 1

suite="$1"
runs="${2:-1}"

# ttytest 的输入流。用例 13 要求执行时 TTY 缓冲区为空，所以前面要留时间让输入被消费完
tty_input()
{
    # OpenSBI 初始化串口时会复位接收 FIFO，开机前送进去的字节会丢
    sleep 2
    printf 'hello\n'
    printf 'a\nb\n'
    printf 'abcdef\n'
    printf 'hi\r'
    printf 'ab\177c\n'
    printf 'ab\010c\n'
    printf '\177\177a\n'
    printf 'junk\025ok\n'
    printf '\004'
    printf 'abc\004'
    printf '\004x\n'
    printf 'block\n'
    sleep 4
    printf 'xy'
    sleep 1
    printf 'z\n'
    printf '%0.sA' $(seq 1 300)
    printf '\025ok2\n'
    printf 'dup\n'
    printf 'fork\n'
    # 用例 25 的 ^C：要等子进程成为 tty 前台组并睡进 read 才打得中
    sleep 3
    printf '\003'
}

# 每套跑完必然出现的收尾标记，缺失即判失败。只选单次 write 内输出的串，多核下不会被插花
suite_marker()
{
    case "$1" in
        sched) echo '======== SCHED TESTS DONE:' ;;
        dcache) echo '=== dcachetest done:' ;;
        vfs)   echo '=== VFS test done:' ;;
        slab)  echo '=== slabtest done:' ;;
        file)  echo '=== filetest done:' ;;
        pipe)  echo '=== pipetest done:' ;;
        tty)   echo '=== ttytest done:' ;;
        mem)   echo '=== memtest done:' ;;
        sig)   echo '=== sigtest done:' ;;
        time)  echo '=== timetest done:' ;;
        seg)   echo '=== segtest done:' ;;
        wait)  echo '=== waittest done:' ;;
        trap)  echo '=== trapkill done:' ;;
        musl)  echo '=== mhello done: ok ===' ;;
        msys)  echo '=== msyscheck done:' ;;
        mroot) echo '=== mrootfs done:' ;;
        mfp)   echo '=== mfptest done:' ;;
        bb)    echo '=== bbtest done ===' ;;
        exec)  echo 'exectest: child reaped, done' ;;
        *)     echo '' ;;
    esac
}

marker=$(suite_marker "$suite")

# 用户程序从 rootfs 镜像加载，镜像比 user/*.elf 或 busybox 旧就重建，否则测的是上一版程序
rootfs_img="build/rootfs.img"
if ls user/*.elf >/dev/null 2>&1; then
    stale=0
    if [ ! -f "$rootfs_img" ]; then
        stale=1
    else
        for e in user/*.elf; do
            if [ "$e" -nt "$rootfs_img" ]; then
                stale=1
                break
            fi
        done
        if [ -f build/busybox ] && [ build/busybox -nt "$rootfs_img" ]; then
            stale=1
        fi
    fi
    if [ "$stale" -eq 1 ]; then
        echo "[$suite] rootfs image missing or stale, rebuilding"
        bash tools/build_rootfs.sh >/dev/null || {
            echo "[$suite] build_rootfs.sh failed" >&2
            exit 1
        }
    fi
fi

for i in $(seq 1 "$runs"); do
    if [ "$suite" == "tty" ]; then
        out=$(tty_input | timeout 90 bash scripts/run.sh 2>&1)
    else
        out=$(timeout 90 bash scripts/run.sh 2>&1)
    fi
    summary=$(echo "$out" | grep -E 'done: [0-9]+ pass|panic|Zombie|shutting down' | tail -3 | tr '\n' ' | ')
    echo "[$suite] run $i: $summary"

    reason=""
    # 排除 RustSBI 关机路径上自己打印的 panic（用 SBI_BIOS 换成 RustSBI 时）
    if echo "$out" | grep -vF '[RustSBI]' | grep -qE 'panic|Zombie'; then
        reason="panic/zombie"
    elif echo "$out" | grep -qE '(  FAIL: |  \[FAIL\] |\] FAIL: |EXEC FAILED)'; then
        reason="assertion failed"
    elif echo "$out" | grep -qE 'done: [0-9]+ pass +[1-9][0-9]* fail'; then
        reason="nonzero fail count"
    elif [ -n "$marker" ] && ! echo "$out" | grep -qF "$marker"; then
        reason="suite did not finish (no '$marker')"
    fi

    if [ -n "$reason" ]; then
        log="/tmp/regress-$suite-$i-fail.log"
        echo "$out" > "$log"
        echo "[$suite] run $i: FAILED ($reason), full log saved to $log"
    fi
done
