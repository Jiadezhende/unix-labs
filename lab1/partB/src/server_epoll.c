/*
 * 系统程序设计 Lab 1 — Part B
 * 基于 epoll 的 I/O 多路复用 echo 服务器 (加分项)
 *
 * 模型: 单线程 reactor + epoll, 水平触发 (LT)。
 *   - epoll 在内核中维护关注集合, 应用无需每轮传入完整 fd 集合,
 *     这是它相对 select/poll 的关键优势。
 *   - 所有 socket 设为非阻塞。
 *   - 监听 fd: 在 epoll_data 中以 NULL 指针标记; 就绪时循环 accept
 *     直到 EAGAIN。
 *   - 客户端 fd: epoll_data 存 conn_t 指针。每个连接带一个输出缓冲,
 *     用于处理"读得快、对端收得慢"的情况。
 *
 * 背压设计: 一次 echo 若未能全部写出, 余下数据存入连接的输出缓冲,
 * 此时关闭该连接的 EPOLLIN (暂停读)、启用 EPOLLOUT; 待缓冲清空后
 * 再恢复 EPOLLIN。这样输出缓冲最多只保存一次读取的数据, 不会因
 * 慢客户端而无限增长。
 *
 * 构建: make
 * 运行: ./bin/server_epoll [-p 端口]
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#define MAX_EVENTS  1024     /* 单次 epoll_wait 返回的事件上限 */
#define MAX_CONNS   4096     /* 同时在线的客户端连接上限 */

/* 单个客户端连接的状态。
 * 输出缓冲只在一次 echo 未写完时使用; 因为缓冲非空期间会关闭
 * EPOLLIN, 所以最多保存一次读取 (<= IO_BUFSIZE 字节) 的数据,
 * 故用定长数组即可, 无需动态扩容。 */
typedef struct {
    int      fd;
    uint32_t events;             /* 当前注册到 epoll 的事件掩码 */
    size_t   outoff;             /* 输出缓冲中已发送的偏移 */
    size_t   outlen;             /* 输出缓冲中数据总长度 */
    char     outbuf[IO_BUFSIZE];
} conn_t;

static int active_conns = 0;     /* 当前在线连接数 (单线程, 无需加锁) */
static int g_work     = 0;       /* -w: 每请求 CPU 忙计算单位 */
static int g_sleep_us = 0;       /* -u: 每请求阻塞 usleep 微秒 */

/* 修改连接注册的 epoll 事件; 若与当前一致则跳过 epoll_ctl。
 * 成功返回 0, 失败返回 -1。 */
static int conn_set_events(int epfd, conn_t *c, uint32_t ev)
{
    if (c->events == ev)
        return 0;
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events   = ev;
    e.data.ptr = c;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &e) < 0)
        return -1;
    c->events = ev;
    return 0;
}

/* 关闭并释放一个连接。先 DEL 再 close, 避免悬空注册。 */
static void conn_close(int epfd, conn_t *c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
    active_conns--;
}

/* 监听 fd 就绪: 循环 accept 直到 EAGAIN, 为每个新连接建 conn_t。 */
static void handle_accept(int epfd, int listen_fd)
{
    for (;;) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                log_msg("accept: fd 耗尽, 暂时跳过");
                break;
            }
            log_msg("accept 失败: %s", strerror(errno));
            break;
        }

        if (active_conns >= MAX_CONNS) {
            log_msg("连接数达上限 %d, 拒绝新连接", MAX_CONNS);
            close(cfd);
            continue;
        }
        if (set_nonblocking(cfd) < 0) {
            log_msg("set_nonblocking 失败, 关闭连接");
            close(cfd);
            continue;
        }

        conn_t *c = calloc(1, sizeof(conn_t));
        if (c == NULL) {
            log_msg("calloc 失败, 关闭连接");
            close(cfd);
            continue;
        }
        c->fd     = cfd;
        c->events = EPOLLIN | EPOLLRDHUP;

        struct epoll_event e;
        memset(&e, 0, sizeof(e));
        e.events   = c->events;
        e.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &e) < 0) {
            log_msg("epoll_ctl ADD 失败: %s", strerror(errno));
            close(cfd);
            free(c);
            continue;
        }
        active_conns++;
    }
}

/* 连接可写: 续发输出缓冲。缓冲清空后恢复 EPOLLIN (解除背压)。
 * 成功返回 0, 出错返回 -1 (调用者负责关闭连接)。 */
static int handle_writable(int epfd, conn_t *c)
{
    while (c->outoff < c->outlen) {
        ssize_t w = write(c->fd, c->outbuf + c->outoff,
                          c->outlen - c->outoff);
        if (w > 0) {
            c->outoff += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;                       /* 内核发送缓冲已满, 下次再发 */
        return -1;                       /* 写出错 */
    }

    if (c->outoff >= c->outlen) {
        c->outoff = c->outlen = 0;
        if (conn_set_events(epfd, c, EPOLLIN | EPOLLRDHUP) < 0)
            return -1;
    }
    return 0;
}

/* 连接可读: 读一次并立即回写。未写完的数据进入输出缓冲并启用
 * 背压。成功返回 0, 需关闭连接时返回 -1。 */
static int handle_readable(int epfd, conn_t *c)
{
    char buf[IO_BUFSIZE];

    ssize_t n = read(c->fd, buf, sizeof(buf));
    if (n == 0)
        return -1;                       /* 对端关闭 */
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;                    /* 暂无数据, LT 会再次通知 */
        return -1;                       /* 读出错 */
    }

    /* 模拟业务处理: 单线程 reactor 下慢任务会卡住整个事件循环, 此时
     * 其它连接的就绪事件全部排队等待 —— 这正是 epoll 在"业务有成本"
     * 场景下需要外接 worker pool 的根本原因。 */
    simulate_work(g_work, g_sleep_us);

    /* 立即回写, 尽量直接发出 */
    size_t off = 0;
    while (off < (size_t)n) {
        ssize_t w = write(c->fd, buf + off, (size_t)n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        return -1;                       /* 写出错 */
    }

    if (off < (size_t)n) {
        /* 未发完: 余下数据存入输出缓冲。进入此分支前 outlen 必为 0
         * (缓冲非空时 EPOLLIN 已关闭), 故可安全地从头写入。 */
        c->outoff = 0;
        c->outlen = (size_t)n - off;
        memcpy(c->outbuf, buf + off, c->outlen);
        /* 启用 EPOLLOUT、暂停 EPOLLIN, 形成背压 */
        if (conn_set_events(epfd, c, EPOLLOUT | EPOLLRDHUP) < 0)
            return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;

    int opt;
    while ((opt = getopt(argc, argv, "p:w:u:h")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            break;
        case 'w':
            g_work = atoi(optarg);
            break;
        case 'u':
            g_sleep_us = atoi(optarg);
            break;
        case 'h':
        default:
            fprintf(stderr,
                "用法: %s [-p 端口] [-w 计算量] [-u 微秒]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    ignore_sigpipe();

    int listen_fd = create_listen_socket(port);
    if (set_nonblocking(listen_fd) < 0)
        die("set_nonblocking(listen_fd)");

    int epfd = epoll_create1(0);
    if (epfd < 0)
        die("epoll_create1");

    /* 监听 socket: data.ptr 置 NULL 作为标记 */
    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    e.events   = EPOLLIN;
    e.data.ptr = NULL;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &e) < 0)
        die("epoll_ctl ADD listen_fd");

    log_msg("epoll 服务器启动 (水平触发 LT), 监听端口 %d "
            "(work=%d, sleep_us=%d)", port, g_work, g_sleep_us);

    struct epoll_event evs[MAX_EVENTS];

    for (;;) {
        int ready = epoll_wait(epfd, evs, MAX_EVENTS, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            die("epoll_wait");
        }

        for (int i = 0; i < ready; i++) {
            conn_t  *c  = evs[i].data.ptr;
            uint32_t re = evs[i].events;

            if (c == NULL) {                 /* 监听 socket */
                handle_accept(epfd, listen_fd);
                continue;
            }

            if (re & (EPOLLERR | EPOLLHUP)) {
                conn_close(epfd, c);
                continue;
            }

            int err = 0;
            if (re & EPOLLOUT)
                err = handle_writable(epfd, c) < 0;
            if (!err && (re & EPOLLIN))
                err = handle_readable(epfd, c) < 0;
            /* 对端半关闭且无待读数据: 直接收尾 */
            if (!err && (re & EPOLLRDHUP) && !(re & EPOLLIN))
                err = 1;

            if (err)
                conn_close(epfd, c);
        }
    }

    return 0;   /* 不会到达 */
}
