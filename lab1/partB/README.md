# Lab 1 — Part B：高吞吐量 I/O 服务器

不同 I/O 模型的并发 echo 服务器实现与性能对比。

## 目录结构

```
partB/
├── src/
│   ├── common.h        公共工具声明
│   ├── common.c        socket 创建 / 非阻塞 / 写满 / 日志 / 忽略 SIGPIPE / 慢任务模拟
│   ├── server_poll.c   基于 poll() 的多路复用 echo 服务器
│   ├── server_pool.c   多线程并发 echo 服务器（thread-per-connection）
│   ├── server_epoll.c  基于 epoll 的多路复用 echo 服务器（加分项）
│   └── client.c        多线程并发压测客户端
├── scripts/
│   ├── bench.sh        参数扫描压测引擎（成本 × 连接 × 并发度，可重复）
│   └── experiments.sh  几套独立实验的编排（各自输出 results_exp*.csv）
├── Makefile
└── README.md
```

## 构建

```sh
make           # 编译全部到 bin/
make clean     # 清理
```

## 服务器

| 可执行文件 | 模型 | 参数 |
|---|---|---|
| `bin/server_poll`  | 单线程 `poll()` 多路复用 | `-p 端口` `-w 计算量` `-u 微秒` |
| `bin/server_pool`  | 多线程并发（每连接一线程，阻塞 I/O） | `-p 端口` `-m 最大连接数` `-w 计算量` `-u 微秒` |
| `bin/server_epoll` | 单线程 `epoll` reactor（LT 模式） | `-p 端口` `-w 计算量` `-u 微秒` |

```sh
./bin/server_poll  -p 9090
./bin/server_pool  -p 9090 -m 4096
./bin/server_epoll -p 9090
```

### 慢任务 / 业务成本旋钮

三种服务器都支持给每个请求附加可配置的“业务处理成本”，把对比从纯
echo 扩展到“存在处理开销”的真实服务器场景。两类慢任务可叠加，均为
`0`（默认）时退化为纯 echo：

| 参数 | 含义 | 默认值 |
|---|---|---|
| `-w` | 每请求 CPU 忙计算单位（占用 CPU，不放弃执行流） | `0` |
| `-u` | 每请求阻塞 `usleep` 微秒（模拟阻塞式慢 I/O） | `0` |

设计目的：单线程事件循环（`poll`/`epoll`）一旦在某个请求上做慢任务，
整个 reactor 都会被卡住，期间其它就绪连接得不到处理；而每连接一线程的
`server_pool` 仍能借多核并行处理多条连接（代价是高连接数下线程资源/调度
压力上升）。这正是后续引入“事件循环 + 工作线程池”混合架构的动机。

```sh
./bin/server_epoll -p 9090 -u 1000   # 每请求阻塞 1ms：单线程吞吐会塌陷
./bin/server_pool  -p 9090 -w 500    # 每请求 500 单位 CPU 计算
```

测试连通性（另开终端）：

```sh
printf 'hello\n' | nc 127.0.0.1 9090   # 应原样回显 hello
```

## 压测客户端

闭环负载模型：每条连接同一时刻只有一个未完成请求，发送一条消息后
收完回显才发下一条。

```sh
./bin/client [-H 主机] [-p 端口] [-c 连接数] [-t 线程数] \
             [-n 总请求数] [-s 消息字节数] [-d 持续秒数]
```

| 参数 | 含义 | 默认值 |
|---|---|---|
| `-H` | 服务器主机 | `127.0.0.1` |
| `-p` | 服务器端口 | `9090` |
| `-c` | 并发连接数（同时打开的 socket 数） | `100` |
| `-t` | 客户端压测线程数（驱动连接的线程） | `4` |
| `-n` | 总请求数（`<=0` 表示不限，仅按 `-d` 停止） | `100000` |
| `-s` | 单条消息字节数 | `64` |
| `-d` | 持续秒数（`>0` 时启用时间上限） | `0` |

输出成功请求数、错误数、吞吐量（req/s、MB/s）与延迟（avg/P50/P95/P99），
并附一行 `CSV,...` 供脚本采集。

```sh
./bin/client -p 9090 -c 500 -t 8 -n 200000 -s 64
```

### `-t` 与 `-c` 是两个不同的轴（重要）

`-c` 条连接平摊到 `-t` 个压测线程，每个线程在自己的连接上做**闭环、一次
一个**：发一条 → 收完回显 → 再发下一条。因此**每个线程在任一时刻只有 1 个
在飞请求**，于是：

- **并发"正在被服务器处理"的请求数 ≈ `-t`（THREADS）**，而不是 `-c`；
- `-c` 多出来的连接大多时间是**空闲打开着**的，考验的是连接持有成本。

| 轴 | 控制什么 | 对各 server 的意义 |
|---|---|---|
| `-t` THREADS | 并发在飞请求数（真正的并行处理压力） | 决定 `pool` 能并行用几个 worker；`epoll`/`poll` 单线程则无所谓 |
| `-c` CONNS | 同时打开的连接数（持有成本） | `poll` 的 O(N) 扫描、`pool` 的每连接线程数都随它涨 |

经验法则：测**并行处理能力**就扫 `-t`；测**海量连接的持有开销**就扫 `-c`
（`-t` 保持适中）。`-t` 还必须够大，否则客户端自身会成为瓶颈、测的就不是
服务器了。

## 批量压测引擎 `bench.sh`

`bench.sh` 是一个参数扫描引擎：沿 **业务成本 × 连接数 × 并发度** 三类轴
扫描，对每个组合重复多次，把每一次运行作为一行（含 `run` 序号）写入结果
CSV，并在末尾打印按组合取均值的汇总表。

```sh
bash scripts/bench.sh                                  # 默认基线（重复 3 次）
# 阻塞型慢任务：扫 sleep 幅度、固定连接、用时间上限
SLEEP_LIST="0 200 500 1000" CONNS_LIST=64 DURATION=10 bash scripts/bench.sh
# 并发度扫描：固定慢任务，扫 THREADS
SLEEP_LIST=500 THREADS_LIST="1 2 4 8 16" CONNS_LIST=64 DURATION=10 bash scripts/bench.sh
```

可用环境变量（均有默认值，可任意组合）：

| 变量 | 含义 | 默认值 |
|---|---|---|
| `WORK_LIST` | CPU 忙计算单位列表（扫业务成本） | `"0"` |
| `SLEEP_LIST` | 阻塞 `usleep` 微秒列表（扫业务成本） | `"0"` |
| `CONNS_LIST` | 连接数列表（扫持有成本） | `"100 500 1000 2000"` |
| `THREADS_LIST` | 客户端线程数列表（扫并发度） | `"8"` |
| `REPEAT` | 每个组合重复运行次数 | `3` |
| `DURATION` | 每次运行持续秒数（`>0` 时改用时间上限，慢任务必备） | `0` |
| `REQS` | 每次运行总请求数（`DURATION>0` 时忽略） | `200000` |
| `MSGSIZE` | 单条消息字节数 | `64` |
| `POOL_MAX` | `server_pool` 的连接上限（`-m`，高连接数实验需调大） | `4096` |
| `SERVERS` | 参与对比的服务器列表 | 三种全测 |
| `OUT` | 结果 CSV 路径 | `results.csv` |
| `PORT` | 监听端口 | `9090` |

结果 CSV 列：`server,work,sleep_us,run,conns,threads,msgsize,requests,errors,elapsed_s,throughput_reqs,mbps,avg_ms,p50_ms,p95_ms,p99_ms`。

> 慢任务（`WORK`/`SLEEP` > 0）下单线程吞吐很低，固定 `REQS` 会让单格耗时极长
> ——务必用 `DURATION` 改为时间上限。高连接数实验前请确认 `ulimit -n` 足够。
> 每次运行会**覆盖** `OUT` 指向的文件，不同实验请用不同 `OUT`（见下）。

## 成套实验 `experiments.sh`

`experiments.sh` 把几套**目的明确、互相独立**的实验固化下来，各自输出到
独立的 `results_exp*.csv`（互不覆盖）：

```sh
bash scripts/experiments.sh        # 跑全部
bash scripts/experiments.sh 2 4    # 只跑实验 2 和 4
```

| 编号 | 实验 | 验证的核心问题 | 输出 |
|---|---|---|---|
| 1 | 纯 echo 基线（扫连接数） | 无业务成本下连接数可扩展性（复现基础对比） | `results_exp1_baseline.csv` |
| 2 | 阻塞型慢任务（扫 sleep） | 单线程被 `usleep` 串行化；`pool` 多线程"同时睡眠"（并行度受 THREADS 限制，不占核） | `results_exp2_blocking.csv` |
| 3 | CPU 型慢任务（扫计算量） | CPU 密集下 `pool` 受核数限制；与实验 2 对比"线程并行 vs 核数并行" | `results_exp3_cpu.csv` |
| 4 | 并发度扫描（扫 threads） | 验证"并发在飞 ≈ THREADS"：`pool` 随 `-t` 上升、单线程持平 | `results_exp4_concurrency.csv` |
| 5 | 高连接数线程压力（扫连接数） | 海量空闲连接下 `pool` 的每连接线程开销拐点；`epoll` 较平 | `results_exp5_thread_pressure.csv` |

> 实验 5 连接数较高，运行前建议 `ulimit -n 65535`。绝对性能依环境而定，
> 分析重在横向对比与变化趋势。

## 进度

- [x] 阶段1：骨架（common 模块 + Makefile + poll 服务器）
- [x] 阶段2：线程池服务器；epoll（加分）
- [x] 阶段3：并发测试客户端 + 压测脚本
- [ ] 阶段4：实验数据采集与分析
- [ ] 阶段5：论文撰写
