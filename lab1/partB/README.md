# Lab 1 — Part B：高吞吐量 I/O 服务器

不同 I/O 模型的并发 echo 服务器实现与性能对比。

## 目录结构

```
partB/
├── src/
│   ├── common.h        公共工具声明
│   ├── common.c        socket 创建 / 非阻塞 / 写满 / 日志 / 忽略 SIGPIPE
│   ├── server_poll.c   基于 poll() 的多路复用 echo 服务器
│   ├── server_pool.c   多线程并发 echo 服务器（thread-per-connection）
│   ├── server_epoll.c  基于 epoll 的多路复用 echo 服务器（加分项）
│   └── client.c        多线程并发压测客户端
├── scripts/
│   └── bench.sh        三种服务器的批量压测脚本
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
| `bin/server_poll`  | 单线程 `poll()` 多路复用 | `-p 端口` |
| `bin/server_pool`  | 多线程并发（每连接一线程，阻塞 I/O） | `-p 端口` `-m 最大连接数` |
| `bin/server_epoll` | 单线程 `epoll` reactor（LT 模式） | `-p 端口` |

```sh
./bin/server_poll  -p 9090
./bin/server_pool  -p 9090 -m 4096
./bin/server_epoll -p 9090
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
| `-c` | 并发连接数 | `100` |
| `-t` | 客户端线程数 | `4` |
| `-n` | 总请求数（`<=0` 表示不限，仅按 `-d` 停止） | `100000` |
| `-s` | 单条消息字节数 | `64` |
| `-d` | 持续秒数（`>0` 时启用时间上限） | `0` |

输出成功请求数、错误数、吞吐量（req/s、MB/s）与延迟（avg/P50/P95/P99），
并附一行 `CSV,...` 供脚本采集。

```sh
./bin/client -p 9090 -c 500 -t 8 -n 200000 -s 64
```

## 批量压测

```sh
bash scripts/bench.sh           # 默认参数
MSGSIZE=1024 CONNS_LIST="100 1000" bash scripts/bench.sh   # 覆盖参数
```

依次启动三种服务器，对每种跑一组并发连接数，结果汇总到 `results.csv`。
环境变量 `MSGSIZE` / `THREADS` / `REQS` / `CONNS_LIST` 可覆盖默认值。

> 高并发测试前请检查 `ulimit -n`，必要时调大；本环境为 WSL2 虚拟化，
> 绝对性能不代表物理机，分析时应关注横向对比与变化趋势。

## 进度

- [x] 阶段1：骨架（common 模块 + Makefile + poll 服务器）
- [x] 阶段2：线程池服务器；epoll（加分）
- [x] 阶段3：并发测试客户端 + 压测脚本
- [ ] 阶段4：实验数据采集与分析
- [ ] 阶段5：论文撰写
