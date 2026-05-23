/*
 * 系统程序设计 Lab 1 — Part B
 * 多线程并发 echo 服务器 (thread-per-connection 模型)
 *
 * 模型: 主线程阻塞 accept(); 每接受一个连接, 就创建一个独立的
 * worker 线程, 由该线程用【阻塞式 I/O】对这条连接做 read/write
 * echo, 直到对端关闭或出错, 线程随即退出。
 *
 * 这是与 poll/epoll 事件驱动相对的【线程并发】模型: 不做 I/O
 * 多路复用, 每条连接独占一个线程及其线程栈。它单连接逻辑直观;
 * 但并发连接数上升时, 线程数量、线程栈内存和上下文切换开销都会
 * 随之增长 —— 这正是要与多路复用模型对比的核心代价。
 *
 * 关于"线程池": 真正复用固定 worker 的线程池, 若 worker 以阻塞
 * I/O 服务一条【持久连接】到结束, 则可并发连接数被钉死为线程数,
 * 多出的连接会饥饿。本实验客户端使用持久连接做闭环压测, 故这里
 * 采用 thread-per-connection, 并以连接数上限 (-m) 防止线程失控。
 *
 * worker 线程以 detached 方式创建并使用较小的线程栈 (echo 仅需
 * 极少栈空间), 以降低高并发下的内存占用。
 *
 * 构建: make
 * 运行: ./bin/server_pool [-p 端口] [-m 最大并发连接数]
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

#define DEFAULT_MAX_CONNS  4096            /* 默认同时在线连接上限 */
#define WORKER_STACK_SIZE  (256 * 1024)    /* 每个 worker 线程栈大小 */

static atomic_int active_conns;            /* 当前在线连接数 */
static int g_work     = 0;                  /* -w: 每请求 CPU 忙计算单位 */
static int g_sleep_us = 0;                  /* -u: 每请求阻塞 usleep 微秒 */

/* worker 线程: 对单个连接做阻塞式 echo, 结束后 close 并退出。 */
static void *worker_main(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char buf[IO_BUFSIZE];

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            /* 模拟业务处理: 每连接独占线程, 慢任务只阻塞自己这条连接,
             * 多条连接的处理可借多核并行 —— 与单线程事件循环形成对照。
             * (g_work/g_sleep_us 在建线程前已确定, worker 只读, 无竞态) */
            simulate_work(g_work, g_sleep_us);
            if (write_all(fd, buf, (size_t)n) < 0)
                break;                      /* 写失败 */
        } else if (n == 0) {
            break;                          /* 对端正常关闭 */
        } else {
            if (errno == EINTR)
                continue;
            break;                          /* 读出错 */
        }
    }

    close(fd);
    atomic_fetch_sub(&active_conns, 1);
    return NULL;
}

int main(int argc, char *argv[])
{
    int port      = DEFAULT_PORT;
    int max_conns = DEFAULT_MAX_CONNS;

    int opt;
    while ((opt = getopt(argc, argv, "p:m:w:u:h")) != -1) {
        switch (opt) {
        case 'p':
            port = atoi(optarg);
            break;
        case 'm':
            max_conns = atoi(optarg);
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
                "用法: %s [-p 端口] [-m 最大并发连接数] [-w 计算量] [-u 微秒]\n",
                argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (max_conns <= 0) {
        fprintf(stderr, "最大并发连接数必须为正整数\n");
        return 1;
    }

    ignore_sigpipe();

    int listen_fd = create_listen_socket(port);

    /* worker 线程属性: detached (无需 join) + 较小线程栈 */
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        die("pthread_attr_init");
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);

    atomic_store(&active_conns, 0);
    log_msg("多线程服务器启动 (thread-per-connection), "
            "监听端口 %d, 连接上限 %d (work=%d, sleep_us=%d)",
            port, max_conns, g_work, g_sleep_us);

    /* 主线程专职 accept, 为每个连接派生一个 worker 线程。
     * active_conns 只由主线程递增、由 worker 递减, 故计数无竞态。 */
    for (;;) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EMFILE || errno == ENFILE) {
                log_msg("accept: fd 耗尽, 暂时跳过");
                continue;
            }
            log_msg("accept 失败: %s", strerror(errno));
            continue;
        }

        if (atomic_load(&active_conns) >= max_conns) {
            log_msg("连接数达上限 %d, 拒绝新连接", max_conns);
            close(cfd);
            continue;
        }

        atomic_fetch_add(&active_conns, 1);
        pthread_t tid;
        if (pthread_create(&tid, &attr, worker_main,
                           (void *)(intptr_t)cfd) != 0) {
            log_msg("pthread_create 失败: %s", strerror(errno));
            close(cfd);
            atomic_fetch_sub(&active_conns, 1);
            continue;
        }
    }

    pthread_attr_destroy(&attr);   /* 不会到达 */
    return 0;
}
