#!/usr/bin/env bash
#
# 系统程序设计 Lab 1 — Part B 实验编排
#
# 调用 bench.sh, 用几套互相独立的参数组合跑几个目的明确的实验, 各自
# 输出到独立的 results_*.csv (互不覆盖)。每个实验都说明"想验证什么"。
#
# 用法:
#   bash scripts/experiments.sh          # 跑全部实验
#   bash scripts/experiments.sh 2 4      # 只跑实验 2 和 4
#
# 公共约定:
#   - 服务器环境为多核 (论文环境 4 vCPU); 闭环负载下并发在飞请求 ≈ THREADS。
#   - 慢任务实验一律用 DURATION (时间上限), 避免单线程吞吐过低时单格耗时爆炸。
#   - 每个组合默认重复 3 次 (REPEAT), 便于看稳定性/取均值。
#   - 高连接数实验请先确认 ulimit -n 足够 (建议 >= 最大连接 + 余量)。

set -u
cd "$(dirname "$0")/.." || exit 1

BENCH="bash scripts/bench.sh"
REPEAT=${REPEAT:-3}

# 解析要跑哪些实验: 不传参数 => 全跑; 传编号 => 只跑列出的
SELECTED=" $* "
run_this() {
    local id=$1
    [ -z "${ARGS_GIVEN:-}" ] && return 0
    case "$SELECTED" in *" $id "*) return 0;; *) return 1;; esac
}
[ "$#" -gt 0 ] && ARGS_GIVEN=1

# 实验 1 — 纯 echo 基线: 连接数可扩展性
# 验证: 无业务成本下, poll 随连接数线性扫描劣化、pool 吃多核、epoll 居中。
if run_this 1; then
    echo "########## 实验 1: 纯 echo 基线 (连接数扫描) ##########"
    OUT=results_exp1_baseline.csv \
    WORK_LIST="0" SLEEP_LIST="0" \
    CONNS_LIST="100 500 1000 2000" THREADS_LIST="8" \
    REQS=200000 REPEAT="$REPEAT" \
    $BENCH
fi

# 实验 2 — 阻塞型慢任务: 扫 sleep 幅度
# 验证: 单线程 reactor 被 usleep 串行化, 吞吐 ≈ 1/sleep 与并发无关;
#       pool 的多条连接线程可"同时睡眠" (睡眠不占核), 并行度受 THREADS 限制。
if run_this 2; then
    echo "########## 实验 2: 阻塞型慢任务 (扫 sleep) ##########"
    OUT=results_exp2_blocking.csv \
    WORK_LIST="0" SLEEP_LIST="0 200 500 1000" \
    CONNS_LIST="64" THREADS_LIST="8" \
    DURATION=10 REPEAT="$REPEAT" \
    $BENCH
fi

# 实验 3 — CPU 型慢任务: 扫计算量
# 验证: CPU 密集处理下 pool 受核数 (≈4) 限制, 单线程 epoll/poll 仅用单核;
#       与实验 2 对比可看出"阻塞型靠线程并行、CPU 型靠核数并行"的差异。
if run_this 3; then
    echo "########## 实验 3: CPU 型慢任务 (扫计算量) ##########"
    OUT=results_exp3_cpu.csv \
    WORK_LIST="0 100 300 500" SLEEP_LIST="0" \
    CONNS_LIST="64" THREADS_LIST="8" \
    DURATION=10 REPEAT="$REPEAT" \
    $BENCH
fi

# 实验 4 — 并发度扫描: 扫 THREADS
# 验证: "并发在飞请求 ≈ THREADS" —— 固定阻塞慢任务, pool 吞吐随 THREADS
#       上升 (线程并行睡眠), 单线程 epoll/poll 基本持平 (串行不受并发影响)。
if run_this 4; then
    echo "########## 实验 4: 并发度扫描 (扫 threads) ##########"
    OUT=results_exp4_concurrency.csv \
    WORK_LIST="0" SLEEP_LIST="500" \
    CONNS_LIST="64" THREADS_LIST="1 2 4 8 16 32" \
    DURATION=10 REPEAT="$REPEAT" \
    $BENCH
fi

# 实验 5 — 高连接数线程压力: 连接数上探
# 验证: 大量 (多为空闲) 连接下, pool 的每连接线程带来内存/调度开销,
#       吞吐随连接数下降并出现拐点; epoll 较平。需调大 POOL_MAX 与 ulimit -n。
if run_this 5; then
    echo "########## 实验 5: 高连接数线程压力 (连接数上探) ##########"
    echo "[experiments] 提示: 本实验连接数高, 请确认 ulimit -n 足够 (如 ulimit -n 65535)"
    OUT=results_exp5_thread_pressure.csv \
    WORK_LIST="0" SLEEP_LIST="0" \
    CONNS_LIST="1000 2000 4000 8000 16000" THREADS_LIST="8" \
    POOL_MAX=20000 REQS=200000 REPEAT="$REPEAT" \
    $BENCH
fi

echo
echo "[experiments] 全部完成。各实验结果:"
ls -1 results_exp*.csv 2>/dev/null
