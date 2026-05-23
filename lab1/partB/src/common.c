/*
 * 系统程序设计 Lab 1 — Part B
 * common.h 中公共工具函数的实现。
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>

void die(const char *msg)
{
    fprintf(stderr, "[致命] %s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

void log_msg(const char *fmt, ...)
{
    char ts[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "[%s] ", ts);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int create_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        die("socket");

    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
        die("setsockopt SO_REUSEADDR");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");
    if (listen(fd, LISTEN_BACKLOG) < 0)
        die("listen");

    return fd;
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p    += w;
        left -= (size_t)w;
    }
    return 0;
}

void ignore_sigpipe(void)
{
    signal(SIGPIPE, SIG_IGN);
}

/* volatile sink: 让忙计算结果"被使用", 防止 -O2 把整个循环优化掉。 */
static volatile uint64_t g_busy_sink;

void simulate_work(int cpu_units, int sleep_us)
{
    if (cpu_units > 0) {
        /* 每单位约 1000 次数据依赖的整数运算; 用上一轮 sink 作为初值,
         * 使循环无法被常量折叠。单位步长便于按机器算力调参。 */
        uint64_t acc = g_busy_sink + 0x9e3779b97f4a7c15ULL;
        for (int u = 0; u < cpu_units; u++) {
            for (int i = 0; i < 1000; i++)
                acc = acc * 6364136223846793005ULL + 1442695040888963407ULL
                    + (uint64_t)i;
        }
        g_busy_sink = acc;
    }
    if (sleep_us > 0)
        usleep((useconds_t)sleep_us);
}
