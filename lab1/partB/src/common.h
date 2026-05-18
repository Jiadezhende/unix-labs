/*
 * 系统程序设计 Lab 1 — Part B
 * 公共工具: socket 创建、非阻塞设置、写满、日志与错误处理
 *
 * 各服务器/客户端源文件都包含本头文件并链接 common.c。
 */
#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <sys/types.h>

#define DEFAULT_PORT     9090     /* 默认监听端口 */
#define LISTEN_BACKLOG   1024     /* listen() 的 backlog 上限 */
#define IO_BUFSIZE       4096     /* echo 读写缓冲区大小 (字节) */

/* 打印 "msg: errno 描述" 到 stderr, 并以 EXIT_FAILURE 退出。 */
void die(const char *msg);

/* 带时间戳的日志, 输出到 stderr (printf 风格, 自动换行)。 */
void log_msg(const char *fmt, ...);

/* 创建 TCP 监听 socket: socket + SO_REUSEADDR + bind + listen。
 * 成功返回监听 fd; 出错直接 die()。 */
int create_listen_socket(int port);

/* 将 fd 设为非阻塞 (O_NONBLOCK)。成功返回 0, 失败返回 -1。 */
int set_nonblocking(int fd);

/* 向 fd 写满 n 字节, 自动处理短写与 EINTR。
 * 成功返回 0, 出错返回 -1。
 * 注意: 仅适用于阻塞 fd; 非阻塞 fd 上遇 EAGAIN 会返回 -1。 */
int write_all(int fd, const void *buf, size_t n);

/* 忽略 SIGPIPE 信号。
 * 不处理时, 向已被对端关闭的连接写数据会使进程直接终止。
 * 所有服务器与客户端在启动时都应调用本函数。 */
void ignore_sigpipe(void);

#endif /* COMMON_H */
