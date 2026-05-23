/*
 * 系统程序设计 Lab 1 — Part B
 * 基于 poll() 的 I/O 多路复用 echo 服务器
 *
 * 模型: 单线程 + poll() 同时监视所有 fd。
 *   - pfds[0]    固定为监听 socket (非阻塞, 便于循环 accept)
 *   - pfds[1..]  已建立的客户端连接 (阻塞模式)
 * poll() 返回后遍历数组: 监听 fd 就绪则 accept, 客户端 fd 就绪则 echo。
 *
 * 阶段1 说明: 客户端 socket 采用阻塞模式, 每次就绪只做一次 read,
 * 实现简单且正确; 慢客户端会造成单线程的队头阻塞, 阶段2 可改为
 * 全非阻塞 + 输出缓冲再做对比。
 *
 * 构建: make
 * 运行: ./bin/server_poll [-p 端口]
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>

#define MAX_FDS  1024     /* 监听 fd + 客户端 fd 的总上限 */

int main(int argc, char *argv[])
{
    int port  = DEFAULT_PORT;
    int work  = 0;     /* -w: 每请求 CPU 忙计算单位 */
    int sleep_us = 0;  /* -u: 每请求阻塞 usleep 微秒 */

    int opt;
    while ((opt = getopt(argc, argv, "p:w:u:h")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            break;
        case 'w':
            work = atoi(optarg);
            break;
        case 'u':
            sleep_us = atoi(optarg);
            break;
        case 'h':
        default:
            fprintf(stderr,
                "用法: %s [-p 端口] [-w 计算量] [-u 微秒]\n", argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    int listen_fd = create_listen_socket(port);
    if (set_nonblocking(listen_fd) < 0)
        die("set_nonblocking(listen_fd)");

    /* pollfd 数组: 槽位空闲时 fd 置 -1 (poll 会忽略负 fd) */
    struct pollfd pfds[MAX_FDS];
    for (int i = 0; i < MAX_FDS; i++) {
        pfds[i].fd      = -1;
        pfds[i].events  = 0;
        pfds[i].revents = 0;
    }
    pfds[0].fd     = listen_fd;
    pfds[0].events = POLLIN;
    int maxidx = 0;          /* 当前使用到的最大下标 */

    log_msg("poll 服务器启动, 监听端口 %d (work=%d, sleep_us=%d)",
            port, work, sleep_us);

    char buf[IO_BUFSIZE];

    for (;;) {
        int ready = poll(pfds, maxidx + 1, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            die("poll");
        }

        /* --- 监听 fd 就绪: 循环 accept 直到 EAGAIN --- */
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept(listen_fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    if (errno == EINTR)
                        continue;
                    log_msg("accept 失败: %s", strerror(errno));
                    break;
                }

                /* 找一个空槽位存放新连接 */
                int slot = -1;
                for (int i = 1; i < MAX_FDS; i++) {
                    if (pfds[i].fd < 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    log_msg("连接数已满, 拒绝新连接");
                    close(cfd);
                    continue;
                }
                pfds[slot].fd      = cfd;
                pfds[slot].events  = POLLIN;
                pfds[slot].revents = 0;
                if (slot > maxidx)
                    maxidx = slot;
            }
        }

        /* --- 客户端 fd 就绪: echo --- */
        for (int i = 1; i <= maxidx; i++) {
            if (pfds[i].fd < 0)
                continue;
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;

            ssize_t n = read(pfds[i].fd, buf, sizeof(buf));
            int closed = 0;
            if (n > 0) {
                /* 模拟业务处理: 单线程下慢任务会阻塞整个事件循环, 期间
                 * 其它就绪连接得不到处理 —— 慢任务对照实验的关键现象。 */
                simulate_work(work, sleep_us);
                if (write_all(pfds[i].fd, buf, (size_t)n) < 0)
                    closed = 1;          /* 写失败 */
            } else if (n == 0) {
                closed = 1;              /* 对端正常关闭 */
            } else if (errno != EINTR) {
                closed = 1;              /* 读出错 */
            }

            if (closed) {
                close(pfds[i].fd);
                pfds[i].fd = -1;
            }
        }

        /* 收缩 maxidx, 避免 poll 扫描已释放的尾部槽位 */
        while (maxidx > 0 && pfds[maxidx].fd < 0)
            maxidx--;
    }

    return 0;   /* 不会到达 */
}
