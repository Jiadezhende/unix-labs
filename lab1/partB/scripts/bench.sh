#!/usr/bin/env bash
#
# 系统程序设计 Lab 1 — Part B 压测脚本
#
# 依次对 poll / 线程池 / epoll 三种服务器, 用同一组并发参数运行
# 压测客户端, 把结果汇总到 results.csv。
#
# 用法:
#   bash scripts/bench.sh
# 可用环境变量覆盖默认参数:
#   MSGSIZE     单条消息字节数        (默认 64)
#   THREADS     客户端线程数          (默认 8)
#   REQS        每组的总请求数        (默认 200000)
#   CONNS_LIST  并发连接数列表        (默认 "100 500 1000 2000")
#   WORK        每请求 CPU 忙计算单位 (默认 0, 即纯 echo)
#   SLEEP_US    每请求阻塞 usleep 微秒(默认 0, 即纯 echo)
#
# 慢任务对照: 设 WORK>0 或 SLEEP_US>0 时, 服务器每个请求附加业务处理
# 成本, 用于观察单线程事件循环 (poll/epoll) 与每连接一线程 (pool) 在
# "存在处理开销" 场景下的可扩展性差异。建议分别跑 WORK=0/SLEEP_US=0
# (基线) 与若干非零取值, 并各自保存 results.csv。
#
# 注意: 实验数据仅供 Part B 分析使用; 本环境为 WSL2 虚拟化,
# 绝对性能不代表物理机, 重点在于横向对比与趋势。

set -u

cd "$(dirname "$0")/.." || exit 1

PORT=9090
MSGSIZE=${MSGSIZE:-64}
THREADS=${THREADS:-8}
REQS=${REQS:-200000}
CONNS_LIST=${CONNS_LIST:-"100 500 1000 2000"}
WORK=${WORK:-0}
SLEEP_US=${SLEEP_US:-0}
OUT=results.csv

echo "[bench] fd 上限 (ulimit -n) = $(ulimit -n)"
echo "[bench] 若并发数较高, 请确保该值足够 (建议 >= 最大并发 + 余量)"

echo "[bench] 编译..."
if ! make >/dev/null; then
    echo "[bench] 编译失败, 退出"
    exit 1
fi

echo "[bench] 业务成本: work=$WORK, sleep_us=$SLEEP_US (均为 0 即纯 echo)"

echo "server,work,sleep_us,conns,threads,msgsize,requests,errors,elapsed_s,throughput_reqs,mbps,avg_ms,p50_ms,p95_ms,p99_ms" > "$OUT"

# run_server <服务器可执行名>
run_server() {
    local bin=$1
    echo
    echo "[bench] ===== $bin ====="

    "./bin/$bin" -p "$PORT" -w "$WORK" -u "$SLEEP_US" >/dev/null 2>&1 &
    local pid=$!
    sleep 1

    if ! kill -0 "$pid" 2>/dev/null; then
        echo "[bench] $bin 启动失败, 跳过"
        return
    fi

    for c in $CONNS_LIST; do
        echo "[bench] $bin: 并发连接数 = $c"
        local line
        line=$(./bin/client -p "$PORT" -c "$c" -t "$THREADS" \
                            -n "$REQS" -s "$MSGSIZE" 2>/dev/null \
               | grep '^CSV,')
        if [ -n "$line" ]; then
            echo "$bin,$WORK,$SLEEP_US,${line#CSV,}" >> "$OUT"
        else
            echo "[bench] 警告: 未取到 $bin (c=$c) 的结果"
        fi
        sleep 1
    done

    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

run_server server_poll
run_server server_pool
run_server server_epoll

echo
echo "[bench] 完成, 结果已写入 $OUT"
echo
column -t -s, "$OUT" 2>/dev/null || cat "$OUT"
