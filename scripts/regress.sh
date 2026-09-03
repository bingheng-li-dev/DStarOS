#!/bin/bash
# 回归套件运行器（容器内使用）：bash scripts/regress.sh <suite> [runs]
#   suite: sched | slab | file | pipe | tty | mem | exec | sig | time
# 切换套件前必须自行改 src/debug/debug.h 里对应的开关（六个 U 态开关互斥）再 make。
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
    # 用例 25（^C → SIGINT）：必须排在最后，且前面留足时间——子进程要先被挪进自己的
    # 进程组、设成 tty 前台组、真正睡进 read，^C 才打得中。
    sleep 3
    printf '\003'
}

# 每套件跑完时必然出现的收尾标记。
#
# **为什么这些字符串是可靠判据**：内核 printf 与 tty_write 都整段持 ConsoleLock，
# 所以**一次 sys_write / 一次 printf 内部的字符串不会被另一个 hart 插花**；插花只发生在
# 相邻两次 write 之间（"  PASS: " / 名字 / "\n" 是三次 write，所以整行不可靠，
# 而下面这些标记都在单次 write 里）。
#
# 没有这条判据的话，"测试跑到一半就死了"会被静默当成通过——pipetest 被新加的 SIGPIPE
# 杀死后，10 次里有 9 次报的是"通过"，只有撞上无关 panic 的那 1 次才留下了日志。
suite_marker()
{
    case "$1" in
        sched) echo '======== SCHED TESTS DONE:' ;;
        slab)  echo '=== slabtest done:' ;;
        file)  echo '=== filetest done:' ;;
        pipe)  echo '=== pipetest done:' ;;
        tty)   echo '=== ttytest done:' ;;
        mem)   echo '=== memtest done:' ;;
        sig)   echo '=== sigtest done:' ;;
        time)  echo '=== timetest done:' ;;
        exec)  echo 'exectest: child reaped, done' ;;
        *)     echo '' ;;
    esac
}

marker=$(suite_marker "$suite")

for i in $(seq 1 "$runs"); do
    if [ "$suite" == "tty" ]; then
        out=$(tty_input | timeout 90 bash scripts/run.sh 2>&1)
    else
        out=$(timeout 90 bash scripts/run.sh 2>&1)
    fi
    summary=$(echo "$out" | grep -E 'done: [0-9]+ pass|panic|Zombie|shutting down' | tail -3 | tr '\n' ' | ')
    echo "[$suite] run $i: $summary"

    reason=""
    # 只认内核自己的 panic：RustSBI 0.4.0 在 sbi_shutdown 之后会打一条自己的
    # "[RustSBI] ERROR - Hart 0 panicked at ..."，那是关机路径的既有噪声，
    # 出现时测试早已跑完，不能算失败。
    if echo "$out" | grep -vF '[RustSBI]' | grep -qE 'panic|Zombie'; then
        reason="panic/zombie"
    # 单条断言的失败标记也在单次 write 里，比数汇总行里的数字可靠
    elif echo "$out" | grep -qE '(  FAIL: |  \[FAIL\] )'; then
        reason="assertion failed"
    elif [ -n "$marker" ] && ! echo "$out" | grep -qF "$marker"; then
        reason="suite did not finish (no '$marker')"
    fi

    # 失败/panic 时把完整串口日志留下来。偶发的 S 态 vmm_segfault 只有拿到 va 和 sepc
    # 才有可能定位，只 grep 汇总行的话现场就没了（2026-08-27 已经因此错过两次）。
    if [ -n "$reason" ]; then
        log="/tmp/regress-$suite-$i-fail.log"
        echo "$out" > "$log"
        echo "[$suite] run $i: FAILED ($reason), full log saved to $log"
    fi
done
