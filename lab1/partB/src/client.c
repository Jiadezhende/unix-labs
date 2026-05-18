/*
 * 系统程序设计 Lab 1 — Part B
 * 多线程并发压测客户端 (TCP echo)
 *
 * 负载模型: 闭环 (closed-loop)。每条连接同一时刻只有一个未完成
 * 请求 —— 发送一条消息后必须收完回显, 才发下一条。这样测得的
 * 延迟即真实往返时间 (RTT), 吞吐量则反映服务器的实际处理能力。
 *
 *   - -c 条连接平均分配到 -t 个客户端线程; 不能整除时, 前若干个
 *     线程各多分一条。
 *   - 每个线程轮流在自己的连接上做同步 send/recv, 记录每次 RTT。
 *   - 达到总请求数 (-n) 或持续时间 (-d) 后停止。
 *   - 汇总所有 RTT, 排序后计算吞吐量与 P50/P95/P99 延迟。
 *
 * 构建: make
 * 运行: ./bin/client [-H 主机] [-p 端口] [-c 连接数] [-t 线程数]
 *                    [-n 总请求数] [-s 消息字节数] [-d 持续秒数]
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

/* --- 全局压测配置 (主线程设置, worker 只读) --- */
static const char *g_host = "127.0.0.1";
static int      g_port        = DEFAULT_PORT;
static long     g_total_reqs  = 100000;   /* -n: <=0 表示不限, 仅按 -d 停止 */
static int      g_msgsize     = 64;       /* -s */
static int      g_duration    = 0;        /* -d: >0 时启用时间上限 (秒) */

static atomic_long g_done;                /* 已完成的成功请求总数 */
static uint64_t    g_deadline_ns;         /* -d>0 时的截止时刻 */

/* 每个 worker 线程的私有状态。RTT 数组线程独立, 避免锁竞争污染测量。 */
typedef struct {
    int       nconns;        /* 本线程负责的连接数 */
    uint64_t *rtts;          /* RTT 样本 (纳秒), 动态扩容 */
    size_t    rtt_count;
    size_t    rtt_cap;
    long      errors;        /* 连接失败 / 收发错误 / 回显不一致 */
} worker_t;

/* 单调时钟当前时刻 (纳秒)。 */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* 建立一条到 host:port 的 TCP 连接 (阻塞模式, 启用 TCP_NODELAY)。
 * 成功返回 fd, 失败返回 -1。 */
static int connect_to(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd >= 0) {
        int on = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    }
    return fd;
}

/* 从 fd 精确读满 n 字节 (处理短读与 EINTR)。
 * 成功返回 0; 对端关闭或出错返回 -1。 */
static int read_n(int fd, void *buf, size_t n)
{
    char *p = buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r > 0) {
            p    += r;
            left -= (size_t)r;
        } else if (r == 0) {
            return -1;               /* 对端关闭 */
        } else {
            if (errno == EINTR)
                continue;
            return -1;               /* 读出错 */
        }
    }
    return 0;
}

/* 是否应停止压测 (总请求数已达上限, 或已到截止时间)。 */
static int should_stop(void)
{
    if (g_total_reqs > 0 && atomic_load(&g_done) >= g_total_reqs)
        return 1;
    if (g_duration > 0 && now_ns() >= g_deadline_ns)
        return 1;
    return 0;
}

/* worker 线程: 建连 -> 轮流在各连接上做闭环 echo -> 记录 RTT。 */
static void *worker_main(void *arg)
{
    worker_t *w = arg;
    if (w->nconns <= 0)
        return NULL;

    int *fds  = malloc((size_t)w->nconns * sizeof(int));
    if (fds == NULL)
        die("malloc");

    int live = 0;
    for (int i = 0; i < w->nconns; i++) {
        fds[i] = connect_to(g_host, g_port);
        if (fds[i] >= 0)
            live++;
        else
            w->errors++;
    }

    char *sbuf = malloc((size_t)g_msgsize);
    char *rbuf = malloc((size_t)g_msgsize);
    if (sbuf == NULL || rbuf == NULL)
        die("malloc");
    memset(sbuf, 'x', (size_t)g_msgsize);

    w->rtt_cap = 1024;
    w->rtts    = malloc(w->rtt_cap * sizeof(uint64_t));
    if (w->rtts == NULL)
        die("malloc");

    int idx = 0;
    while (live > 0 && !should_stop()) {
        /* 找到下一条仍存活的连接 */
        if (fds[idx] < 0) {
            idx = (idx + 1) % w->nconns;
            continue;
        }
        int fd = fds[idx];

        uint64_t t0 = now_ns();
        int ok = (write_all(fd, sbuf, (size_t)g_msgsize) == 0)
              && (read_n(fd, rbuf, (size_t)g_msgsize) == 0);
        uint64_t t1 = now_ns();

        if (!ok || memcmp(sbuf, rbuf, (size_t)g_msgsize) != 0) {
            w->errors++;
            close(fd);
            fds[idx] = -1;
            live--;
            idx = (idx + 1) % w->nconns;
            continue;
        }

        if (w->rtt_count == w->rtt_cap) {
            w->rtt_cap *= 2;
            uint64_t *p = realloc(w->rtts, w->rtt_cap * sizeof(uint64_t));
            if (p == NULL)
                die("realloc");
            w->rtts = p;
        }
        w->rtts[w->rtt_count++] = t1 - t0;
        atomic_fetch_add(&g_done, 1);

        idx = (idx + 1) % w->nconns;
    }

    for (int i = 0; i < w->nconns; i++) {
        if (fds[i] >= 0)
            close(fds[i]);
    }
    free(fds);
    free(sbuf);
    free(rbuf);
    return NULL;
}

/* qsort 比较函数: uint64_t 升序。 */
static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* 取排序后数组的第 pct 百分位 (pct ∈ [0,100))。 */
static double percentile_ms(const uint64_t *sorted, size_t n, double pct)
{
    if (n == 0)
        return 0.0;
    size_t i = (size_t)(pct / 100.0 * (double)n);
    if (i >= n)
        i = n - 1;
    return (double)sorted[i] / 1e6;
}

int main(int argc, char *argv[])
{
    int nconns  = 100;
    int nthreads = 4;

    int opt;
    while ((opt = getopt(argc, argv, "H:p:c:t:n:s:d:h")) != -1) {
        switch (opt) {
        case 'H': g_host       = optarg;        break;
        case 'p': g_port       = atoi(optarg);  break;
        case 'c': nconns       = atoi(optarg);  break;
        case 't': nthreads     = atoi(optarg);  break;
        case 'n': g_total_reqs = atol(optarg);  break;
        case 's': g_msgsize    = atoi(optarg);  break;
        case 'd': g_duration   = atoi(optarg);  break;
        case 'h':
        default:
            fprintf(stderr,
                "用法: %s [-H 主机] [-p 端口] [-c 连接数] [-t 线程数]\n"
                "          [-n 总请求数] [-s 消息字节数] [-d 持续秒数]\n",
                argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }
    if (nconns <= 0 || nthreads <= 0 || g_msgsize <= 0) {
        fprintf(stderr, "连接数 / 线程数 / 消息大小必须为正整数\n");
        return 1;
    }
    if (g_total_reqs <= 0 && g_duration <= 0) {
        fprintf(stderr, "必须指定 -n (总请求数) 或 -d (持续秒数) 其中之一\n");
        return 1;
    }
    if (nthreads > nconns)
        nthreads = nconns;          /* 线程数不超过连接数 */

    ignore_sigpipe();

    /* 连接分配: 前 nconns%nthreads 个线程各多分一条 */
    worker_t  *workers = calloc((size_t)nthreads, sizeof(worker_t));
    pthread_t *tids    = malloc((size_t)nthreads * sizeof(pthread_t));
    if (workers == NULL || tids == NULL)
        die("malloc");

    int base = nconns / nthreads;
    int rem  = nconns % nthreads;
    for (int i = 0; i < nthreads; i++)
        workers[i].nconns = base + (i < rem ? 1 : 0);

    log_msg("压测开始: 目标 %s:%d, 连接 %d, 线程 %d, 消息 %d 字节",
            g_host, g_port, nconns, nthreads, g_msgsize);

    atomic_store(&g_done, 0);
    uint64_t wall_start = now_ns();
    if (g_duration > 0)
        g_deadline_ns = wall_start + (uint64_t)g_duration * 1000000000ULL;

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, worker_main, &workers[i]) != 0)
            die("pthread_create");
    }
    for (int i = 0; i < nthreads; i++)
        pthread_join(tids[i], NULL);

    uint64_t wall_end = now_ns();
    double elapsed = (double)(wall_end - wall_start) / 1e9;
    if (elapsed <= 0.0)
        elapsed = 1e-9;

    /* 合并各线程 RTT 样本 */
    size_t total = 0;
    long   errors = 0;
    for (int i = 0; i < nthreads; i++) {
        total  += workers[i].rtt_count;
        errors += workers[i].errors;
    }

    uint64_t *all = NULL;
    uint64_t  sum = 0;
    if (total > 0) {
        all = malloc(total * sizeof(uint64_t));
        if (all == NULL)
            die("malloc");
        size_t off = 0;
        for (int i = 0; i < nthreads; i++) {
            memcpy(all + off, workers[i].rtts,
                   workers[i].rtt_count * sizeof(uint64_t));
            off += workers[i].rtt_count;
        }
        for (size_t i = 0; i < total; i++)
            sum += all[i];
        qsort(all, total, sizeof(uint64_t), cmp_u64);
    }

    double thr_reqs = (double)total / elapsed;
    double mbps     = (double)total * (double)g_msgsize / elapsed / 1e6;
    double avg_ms   = total ? (double)sum / (double)total / 1e6 : 0.0;
    double p50 = percentile_ms(all, total, 50.0);
    double p95 = percentile_ms(all, total, 95.0);
    double p99 = percentile_ms(all, total, 99.0);

    printf("\n===== 压测结果 =====\n");
    printf("成功请求 : %zu\n", total);
    printf("错误次数 : %ld\n", errors);
    printf("耗时     : %.3f s\n", elapsed);
    printf("吞吐量   : %.0f req/s   (%.2f MB/s, 仅计回显负载)\n",
           thr_reqs, mbps);
    printf("延迟(ms) : avg %.3f  P50 %.3f  P95 %.3f  P99 %.3f\n",
           avg_ms, p50, p95, p99);
    /* 供 bench.sh 采集的单行 CSV (字段见表头注释) */
    printf("CSV,%d,%d,%d,%zu,%ld,%.3f,%.0f,%.2f,%.3f,%.3f,%.3f,%.3f\n",
           nconns, nthreads, g_msgsize, total, errors, elapsed,
           thr_reqs, mbps, avg_ms, p50, p95, p99);

    for (int i = 0; i < nthreads; i++)
        free(workers[i].rtts);
    free(all);
    free(workers);
    free(tids);
    return 0;
}
