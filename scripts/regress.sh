#!/bin/bash
# 回归套件运行器（容器内使用）：bash scripts/regress.sh <suite> [runs]
#   suite: sched | slab | file | pipe | tty | mem | exec
# 切换套件前必须自行改 src/debug/debug.h 里对应的开关（五个 U 态开关互斥）再 make。
cd "$(dirname "$0")/.." || exit 1

suite="$1"
runs="${2:-1}"

# ttytest 的输入流：按用例调用顺序拼成一条喂给 QEMU stdin。
# 用例 13（O_NONBLOCK 空读期望 -EAGAIN）要求它执行的那一刻 TTY 缓冲区为空，
# 所以在它之前必须留出足够时间让前面的输入被消费干净，不能一次性全灌进去。
tty_input()
{
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
}

for i in $(seq 1 "$runs"); do
    if [ "$suite" == "tty" ]; then
        out=$(tty_input | timeout 90 bash scripts/run.sh 2>&1)
    else
        out=$(timeout 90 bash scripts/run.sh 2>&1)
    fi
    summary=$(echo "$out" | grep -E 'done: [0-9]+ pass|panic|Zombie|shutting down' | tail -3 | tr '\n' ' | ')
    echo "[$suite] run $i: $summary"
    # 失败/panic 时把完整串口日志留下来。偶发的 S 态 vmm_segfault 只有拿到 va 和 sepc
    # 才有可能定位，只 grep 汇总行的话现场就没了（2026-08-27 已经因此错过两次）。
    if echo "$summary" | grep -qE 'panic|Zombie' || echo "$out" | grep -qE '[1-9][0-9]* fail'; then
        log="/tmp/regress-$suite-$i-fail.log"
        echo "$out" > "$log"
        echo "[$suite] run $i: FAILED, full log saved to $log"
    fi
done
