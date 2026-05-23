#!/usr/bin/env bash
#
# 系统程序设计 Lab 1 — Part B 压测引擎
#
# 对 poll / 线程池 / epoll 三种服务器做参数扫描压测, 把每次运行作为
# 一行追加到结果 CSV。支持沿四个维度扫描, 并对每个参数组合重复多次:
#
#   业务成本   WORK_LIST   每请求 CPU 忙计算单位        (默认 "0")
#              SLEEP_LIST  每请求阻塞 usleep 微秒        (默认 "0")
#   连接持有   CONNS_LIST  同时打开的 socket 数         (默认 "100 500 1000 2000")
#   并发在飞   THREADS_LIST 客户端压测线程数            (默认 "8")
#   重复       REPEAT      每个组合重复运行次数          (默认 3)
#
# 说明:闭环负载下每个客户端线程同一时刻只有 1 个在飞请求, 所以
#   - 并发"在被处理"的请求数 ≈ THREADS (真正的并行处理压力);
#   - CONNS 多出来的连接大多空闲打开着 (考验连接持有成本: poll 的
#     O(N) 扫描、pool 的每连接线程数)。
# 因此 THREADS 与 CONNS 是两个不同的轴, 分别扫描才能讲清楚模型差异。
#
# 其它可调参数:
#   MSGSIZE    单条消息字节数         (默认 64)
#   REQS       每次运行的总请求数     (默认 200000; DURATION>0 时忽略)
#   DURATION   每次运行的持续秒数     (默认 0 = 用 REQS; 慢任务建议设为 10)
#   POOL_MAX   server_pool 的连接上限 (默认 4096; 高连接数实验需调大)
#   SERVERS    参与对比的服务器列表   (默认 "server_poll server_pool server_epoll")
#   OUT        结果 CSV 路径          (默认 results.csv)
#   PORT       监听端口               (默认 9090)
#
# 用法:
#   bash scripts/bench.sh                               # 默认基线
#   SLEEP_LIST="0 200 500 1000" CONNS_LIST=64 DURATION=10 bash scripts/bench.sh
#   预设的几套独立实验见 scripts/experiments.sh
#
# 注意: 慢任务 (WORK/SLEEP>0) 下单线程吞吐会很低, 固定 REQS 会让单格
# 耗时极长 —— 此时务必用 DURATION 改为时间上限。高连接数实验请先确认
# ulimit -n 足够。绝对性能依环境而定, 分析重在横向对比与趋势。

set -u

cd "$(dirname "$0")/.." || exit 1

PORT=${PORT:-9090}
MSGSIZE=${MSGSIZE:-64}
REQS=${REQS:-200000}
DURATION=${DURATION:-0}
REPEAT=${REPEAT:-3}
CONNS_LIST=${CONNS_LIST:-"100 500 1000 2000"}
THREADS_LIST=${THREADS_LIST:-"8"}
WORK_LIST=${WORK_LIST:-"0"}
SLEEP_LIST=${SLEEP_LIST:-"0"}
POOL_MAX=${POOL_MAX:-4096}
SERVERS=${SERVERS:-"server_poll server_pool server_epoll"}
OUT=${OUT:-results.csv}

echo "[bench] fd 上限 (ulimit -n) = $(ulimit -n)"
echo "[bench] 编译..."
# 只构建本次会用到的服务器 + 客户端 (便于在不支持某 server 的平台上跑子集)
BUILD_TARGETS="bin/client"
for s in $SERVERS; do BUILD_TARGETS="$BUILD_TARGETS bin/$s"; done
if ! make $BUILD_TARGETS >/dev/null; then
    echo "[bench] 编译失败, 退出"
    exit 1
fi

# 负载停止条件: DURATION>0 用时间上限, 否则用总请求数
if [ "$DURATION" -gt 0 ]; then
    STOP_ARG="-d $DURATION"
    echo "[bench] 负载: 每次运行 ${DURATION}s (时间上限)"
else
    STOP_ARG="-n $REQS"
    echo "[bench] 负载: 每次运行 ${REQS} 请求"
fi
echo "[bench] 扫描: work={$WORK_LIST} sleep_us={$SLEEP_LIST}"
echo "[bench]       conns={$CONNS_LIST} threads={$THREADS_LIST} repeat=$REPEAT"
echo "[bench] 输出: $OUT"

echo "server,work,sleep_us,run,conns,threads,msgsize,requests,errors,elapsed_s,throughput_reqs,mbps,avg_ms,p50_ms,p95_ms,p99_ms" > "$OUT"

# run_matrix <服务器名> <work> <sleep_us>
# 用给定业务成本启动一次服务器, 对 conns × threads × repeat 跑客户端。
run_matrix() {
    local bin=$1 work=$2 sleep_us=$3
    local extra=""
    [ "$bin" = "server_pool" ] && extra="-m $POOL_MAX"

    "./bin/$bin" -p "$PORT" -w "$work" -u "$sleep_us" $extra \
        >/dev/null 2>&1 &
    local pid=$!
    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "[bench] $bin (work=$work sleep=$sleep_us) 启动失败, 跳过"
        return
    fi

    local c t run line
    for c in $CONNS_LIST; do
        for t in $THREADS_LIST; do
            for run in $(seq 1 "$REPEAT"); do
                echo "[bench] $bin work=$work sleep=$sleep_us c=$c t=$t run=$run"
                line=$(./bin/client -p "$PORT" -c "$c" -t "$t" \
                                    $STOP_ARG -s "$MSGSIZE" 2>/dev/null \
                       | grep '^CSV,')
                if [ -n "$line" ]; then
                    echo "$bin,$work,$sleep_us,$run,${line#CSV,}" >> "$OUT"
                else
                    echo "[bench] 警告: 未取到结果 ($bin c=$c t=$t run=$run)"
                fi
            done
        done
    done

    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
}

for work in $WORK_LIST; do
    for sleep_us in $SLEEP_LIST; do
        for bin in $SERVERS; do
            echo
            echo "[bench] ===== $bin (work=$work sleep_us=$sleep_us) ====="
            run_matrix "$bin" "$work" "$sleep_us"
        done
    done
done

echo
echo "[bench] 完成, 原始结果 ($(($(wc -l < "$OUT") - 1)) 行) 写入 $OUT"

# 各参数组合的均值汇总 (对 repeat 取平均吞吐/平均延迟, 错误求和)
echo
echo "[bench] ===== 均值汇总 (按 server,work,sleep_us,conns,threads) ====="
awk -F, 'NR>1 {
        k=$1","$2","$3","$5","$6;
        thr[k]+=$11; avg[k]+=$13; err[k]+=$9; n[k]++;
    }
    END {
        print "server,work,sleep_us,conns,threads,runs,mean_thr_reqs,mean_avg_ms,sum_errors";
        for (k in n)
            printf "%s,%d,%.0f,%.3f,%d\n", k, n[k], thr[k]/n[k], avg[k]/n[k], err[k];
    }' "$OUT" | sort -t, -k1,1 -k2,2n -k3,3n -k4,4n -k5,5n \
  | column -t -s,
