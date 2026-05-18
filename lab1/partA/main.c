/*
 * 系统程序设计 Lab 1 — Part A
 * UNIX I/O 基础性能对比测试程序
 *
 * 程序流程:
 *   ① 确定版本环境  —— detect_environment()
 *   ② 分别调用测试函数 —— test_read / test_getc / test_fgetc /
 *                          test_fread / test_my_fread / test_write
 *   ③ 聚合并输出结果 —— 终端对齐表格 + CSV 文件
 *
 * 构建: make
 * 运行: ./ioperf [-h 查看选项]
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/sysmacros.h>

/* ===================== 常量定义 ===================== */

/* 实验要求的 18 种 BUFFSIZE (字节) */
static const size_t BUFFSIZES[] = {
    1, 2, 4, 8, 16, 32, 64, 128, 256, 512,
    1024, 2048, 4096, 8192, 16384, 32768, 65536, 16777216
};
#define N_BUFFSIZES ((int)(sizeof(BUFFSIZES) / sizeof(BUFFSIZES[0])))

#define MB              (1024L * 1024L)
#define DEF_READ_MB     512        /* 默认读测试文件大小 */
#define DEF_WRITE_MB    256        /* 默认无 O_SYNC 写入总量 */
#define DEF_OSYNC_MB    16         /* 默认 O_SYNC 写入总量 */
#define MYFREAD_BUFSIZE BUFSIZ     /* my_fread 内部缓冲区大小, 固定 */

/*
 * O_SYNC 下每次 write() 都强制落盘, 比普通写慢几个数量级。
 * 若按字节总量测最小 BUFFSIZE, 写入次数会达千万级, 耗时不可接受。
 * 因此 O_SYNC 测试为每个 BUFFSIZE 限制最大 write() 次数, 实际写入量
 * 见结果表的吞吐量与 CSV 的 bytes 列。
 */
#define OSYNC_MAX_WRITES 4096

#define SEP "----------------------------------------------------------------------"

/* ===================== 数据结构 ===================== */

/* 一次测试的三种时间 (秒) */
typedef struct {
    double real;   /* 墙钟时间 */
    double user;   /* 用户态 CPU 时间 */
    double sys;    /* 内核态 CPU 时间 */
} timing_t;

/* 计时快照: 墙钟 + 资源使用 */
typedef struct {
    struct timespec wall;
    struct rusage   ru;
} snap_t;

/* 单次测试结果 */
typedef struct {
    size_t   buffsize;
    timing_t t;
    long     bytes;    /* 实际处理字节数 */
} result_t;

/* my_fread 使用的"文件对象", 仿 FILE */
typedef struct {
    int            fd;
    unsigned char *buf;
    size_t         bufsize;   /* 内部缓冲区容量 */
    size_t         pos;       /* 下一个待读字节在 buf 中的下标 */
    size_t         len;       /* buf 中有效字节数 */
    int            eof;
} MYFILE;

/* ===================== 全局选项 ===================== */

static int   g_drop_cache = 0;     /* -d: 读测试前逐出文件页缓存 */
static FILE *g_csv        = NULL;  /* 汇总 CSV 文件句柄 */

/* ===================== 通用辅助 ===================== */

static void die(const char *msg)
{
    fprintf(stderr, "错误: %s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p)
        die("内存分配失败");
    return p;
}

/* 取出 path 所在目录, 写入 out (path 无目录分量时返回 ".") */
static void path_dir(const char *path, char *out, size_t outsz)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, outsz, ".");
        return;
    }
    size_t n = (size_t)(slash - path);
    if (n == 0)
        n = 1;                 /* 根目录 "/" */
    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* 判断文本文件是否含某子串 (用于读 /proc/cpuinfo 等) */
static int file_contains(const char *path, const char *needle)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    fclose(fp);
    return found;
}

/* ===================== 计时 ===================== */

static void take_snapshot(snap_t *s)
{
    clock_gettime(CLOCK_MONOTONIC, &s->wall);
    getrusage(RUSAGE_SELF, &s->ru);
}

static double tv_sec(const struct timeval *tv)
{
    return (double)tv->tv_sec + (double)tv->tv_usec / 1e6;
}

/* 由前后两个快照计算耗时 */
static timing_t compute_timing(const snap_t *a, const snap_t *b)
{
    timing_t t;
    t.real = (double)(b->wall.tv_sec - a->wall.tv_sec)
           + (double)(b->wall.tv_nsec - a->wall.tv_nsec) / 1e9;
    t.user = tv_sec(&b->ru.ru_utime) - tv_sec(&a->ru.ru_utime);
    t.sys  = tv_sec(&b->ru.ru_stime) - tv_sec(&a->ru.ru_stime);
    return t;
}

static double throughput_mbps(long bytes, double real)
{
    if (real <= 0.0)
        return 0.0;
    return (double)bytes / real / (1024.0 * 1024.0);
}

/* 调用 posix_fadvise 逐出指定文件的页缓存, 让下次读取尽量"冷启动" */
static void drop_file_cache(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
}

/* ===================== ① 环境检测 ===================== */

static const char *fs_type_name(unsigned long t)
{
    switch (t) {
    case 0xEF53:     return "ext2/3/4";
    case 0x58465342: return "XFS";
    case 0x9123683E: return "Btrfs";
    case 0x01021994: return "tmpfs";
    case 0x01021997: return "v9fs (Plan 9)";
    case 0x6969:     return "NFS";
    case 0xFF534D42: return "CIFS/SMB";
    case 0x4D44:     return "FAT/msdos";
    case 0x65735546: return "FUSE";
    case 0x794C7630: return "overlayfs";
    case 0x52654973: return "ReiserFS";
    case 0x2FC12FC1: return "ZFS";
    default:         return "未知";
    }
}

static void detect_disk_type(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) != 0) {
        printf("  磁盘类型     : N/A (stat 失败: %s)\n", strerror(errno));
        return;
    }
    unsigned maj = major(st.st_dev);
    unsigned min = minor(st.st_dev);

    char p[256];
    FILE *fp;

    /* 先尝试设备本身, 再尝试其父设备 (分区 -> 整盘) */
    snprintf(p, sizeof(p), "/sys/dev/block/%u:%u/queue/rotational", maj, min);
    fp = fopen(p, "r");
    if (!fp) {
        snprintf(p, sizeof(p),
                 "/sys/dev/block/%u:%u/../queue/rotational", maj, min);
        fp = fopen(p, "r");
    }
    if (!fp) {
        printf("  磁盘类型     : N/A (无法读取 rotational, 设备号 %u:%u)\n",
               maj, min);
        return;
    }
    int rot = -1;
    if (fscanf(fp, "%d", &rot) != 1)
        rot = -1;
    fclose(fp);

    if (rot == 0)
        printf("  磁盘类型     : SSD / 非旋转设备 (rotational=0, 设备号 %u:%u)\n",
               maj, min);
    else if (rot == 1)
        printf("  磁盘类型     : HDD / 旋转磁盘 (rotational=1, 设备号 %u:%u)\n",
               maj, min);
    else
        printf("  磁盘类型     : N/A (设备号 %u:%u)\n", maj, min);
}

static void detect_environment(const char *path)
{
    char dir[PATH_MAX];
    path_dir(path, dir, sizeof(dir));

    printf("%s\n", SEP);
    printf(" ① 实验环境信息\n");
    printf("%s\n", SEP);

    struct utsname u;
    memset(&u, 0, sizeof(u));
    if (uname(&u) == 0) {
        printf("  操作系统     : %s\n", u.sysname);
        printf("  内核版本     : %s\n", u.release);
        printf("  内核构建     : %s\n", u.version);
        printf("  硬件架构     : %s\n", u.machine);
        printf("  主机名       : %s\n", u.nodename);
    } else {
        printf("  操作系统     : (uname 失败: %s)\n", strerror(errno));
    }

    /* 虚拟化 / WSL 判断 */
    if (strstr(u.release, "microsoft") || strstr(u.release, "WSL") ||
        strstr(u.version, "microsoft") || strstr(u.version, "WSL")) {
        printf("  运行环境     : WSL2 (Windows Subsystem for Linux, 虚拟化环境)\n");
    } else if (file_contains("/proc/cpuinfo", "hypervisor")) {
        printf("  运行环境     : 虚拟机 (检测到 hypervisor CPU 标志)\n");
    } else {
        printf("  运行环境     : 物理机 (未检测到虚拟化标志)\n");
    }

    /* 文件系统类型 */
    struct statfs sfs;
    if (statfs(dir, &sfs) == 0) {
        printf("  文件系统     : %s (magic 0x%lx, 路径 %s)\n",
               fs_type_name((unsigned long)sfs.f_type),
               (unsigned long)sfs.f_type, dir);
        printf("  文件系统块大小: %ld 字节\n", (long)sfs.f_bsize);
    } else {
        printf("  文件系统     : (statfs 失败: %s)\n", strerror(errno));
    }

    /* 磁盘类型 */
    detect_disk_type(dir);

    /* 其它系统参数 */
    printf("  CPU 核心数   : %ld\n", sysconf(_SC_NPROCESSORS_ONLN));
    printf("  内存页大小   : %ld 字节\n", sysconf(_SC_PAGESIZE));
}

/* ===================== 测试文件准备 ===================== */

static void ensure_test_file(const char *path, long size_mb)
{
    off_t want = (off_t)size_mb * MB;

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size == want) {
        printf("测试文件已存在且大小匹配: %s (%ld MB)\n", path, size_mb);
        return;
    }

    printf("生成测试文件: %s (%ld MB) ... ", path, size_mb);
    fflush(stdout);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die("无法创建测试文件");

    size_t chunk = (size_t)MB;
    unsigned char *buf = xmalloc(chunk);
    /* 填充伪随机模式, 避免文件系统对全零数据做特殊优化 */
    unsigned long seed = 0x12345678UL;
    for (size_t i = 0; i < chunk; i++) {
        seed = seed * 1103515245UL + 12345UL;
        buf[i] = (unsigned char)(seed >> 16);
    }

    off_t written = 0;
    while (written < want) {
        size_t n = chunk;
        if ((off_t)n > want - written)
            n = (size_t)(want - written);
        ssize_t w = write(fd, buf, n);
        if (w < 0)
            die("写测试文件失败");
        written += w;
    }
    free(buf);
    if (fsync(fd) < 0)
        die("fsync 失败");
    close(fd);
    printf("完成\n");
}

/* ===================== ② 测试函数 ===================== */

/* 2.2.1  read() 系统调用 */
static timing_t test_read(const char *path, size_t buffsize, long *bytes_out)
{
    if (g_drop_cache)
        drop_file_cache(path);

    unsigned char *buf = xmalloc(buffsize);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        die("打开测试文件失败");

    snap_t s0, s1;
    long total = 0;
    ssize_t n;
    take_snapshot(&s0);
    while ((n = read(fd, buf, buffsize)) > 0)
        total += n;
    take_snapshot(&s1);
    if (n < 0)
        die("read 失败");

    close(fd);
    free(buf);
    *bytes_out = total;
    return compute_timing(&s0, &s1);
}

/* 2.2.2  stdio getc() 宏 — 逐字符读取 */
static timing_t test_getc(const char *path, long *bytes_out)
{
    if (g_drop_cache)
        drop_file_cache(path);

    FILE *fp = fopen(path, "rb");
    if (!fp)
        die("fopen 失败");

    snap_t s0, s1;
    long total = 0;
    int c;
    take_snapshot(&s0);
    while ((c = getc(fp)) != EOF)
        total++;
    take_snapshot(&s1);

    fclose(fp);
    *bytes_out = total;
    return compute_timing(&s0, &s1);
}

/* 2.2.2  stdio fgetc() 函数 — 逐字符读取 */
static timing_t test_fgetc(const char *path, long *bytes_out)
{
    if (g_drop_cache)
        drop_file_cache(path);

    FILE *fp = fopen(path, "rb");
    if (!fp)
        die("fopen 失败");

    snap_t s0, s1;
    long total = 0;
    int c;
    take_snapshot(&s0);
    while ((c = fgetc(fp)) != EOF)
        total++;
    take_snapshot(&s1);

    fclose(fp);
    *bytes_out = total;
    return compute_timing(&s0, &s1);
}

/* 2.2.3  fread() 库函数 */
static timing_t test_fread(const char *path, size_t buffsize, long *bytes_out)
{
    if (g_drop_cache)
        drop_file_cache(path);

    unsigned char *buf = xmalloc(buffsize);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        die("fopen 失败");

    snap_t s0, s1;
    long total = 0;
    size_t n;
    take_snapshot(&s0);
    while ((n = fread(buf, 1, buffsize, fp)) > 0)
        total += (long)n;
    take_snapshot(&s1);

    fclose(fp);
    free(buf);
    *bytes_out = total;
    return compute_timing(&s0, &s1);
}

/* ---- 2.2.4  my_fread 实现: 仿 fread, 内部基于 read() 维护用户态缓冲区 ---- */

static MYFILE *myfopen(const char *path, size_t bufsize)
{
    MYFILE *mf = xmalloc(sizeof(*mf));
    mf->fd = open(path, O_RDONLY);
    if (mf->fd < 0)
        die("my_fread: 打开文件失败");
    mf->buf     = xmalloc(bufsize);
    mf->bufsize = bufsize;
    mf->pos     = 0;
    mf->len     = 0;
    mf->eof     = 0;
    return mf;
}

/*
 * my_fread: 语义仿标准库 fread。
 * 从内部缓冲区拷贝数据; 仅当缓冲区耗尽时才调用一次 read() 补充。
 * 返回成功读取的完整成员数。
 */
static size_t my_fread(void *ptr, size_t size, size_t nmemb, MYFILE *mf)
{
    if (size == 0 || nmemb == 0)
        return 0;

    size_t want = size * nmemb;       /* 期望读取的字节数 */
    size_t got  = 0;
    unsigned char *dst = ptr;

    while (got < want) {
        if (mf->pos == mf->len) {     /* 缓冲区已空 */
            if (mf->eof)
                break;
            ssize_t n = read(mf->fd, mf->buf, mf->bufsize);
            if (n < 0)
                die("my_fread: read 失败");
            if (n == 0) {
                mf->eof = 1;
                break;
            }
            mf->pos = 0;
            mf->len = (size_t)n;
        }
        size_t avail = mf->len - mf->pos;
        size_t need  = want - got;
        size_t cpy   = (avail < need) ? avail : need;
        memcpy(dst + got, mf->buf + mf->pos, cpy);
        mf->pos += cpy;
        got     += cpy;
    }
    return got / size;                /* 只计完整成员 */
}

static void myfclose(MYFILE *mf)
{
    close(mf->fd);
    free(mf->buf);
    free(mf);
}

/* 2.2.4  my_fread 测试: 扫描的 BUFFSIZE 作为每次请求大小, 内部缓冲固定 */
static timing_t test_my_fread(const char *path, size_t reqsize, long *bytes_out)
{
    if (g_drop_cache)
        drop_file_cache(path);

    unsigned char *buf = xmalloc(reqsize);
    MYFILE *mf = myfopen(path, MYFREAD_BUFSIZE);

    snap_t s0, s1;
    long total = 0;
    size_t n;
    take_snapshot(&s0);
    while ((n = my_fread(buf, 1, reqsize, mf)) > 0)
        total += (long)n;
    take_snapshot(&s1);

    myfclose(mf);
    free(buf);
    *bytes_out = total;
    return compute_timing(&s0, &s1);
}

/* 2.2.5 / 2.2.6  write() 测试 (use_osync 控制是否带 O_SYNC) */
static timing_t test_write(const char *path, size_t buffsize, int use_osync,
                           long total_bytes, long max_ops, long *bytes_out)
{
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (use_osync)
        flags |= O_SYNC;

    int fd = open(path, flags, 0644);
    if (fd < 0)
        die("打开写测试文件失败");

    unsigned char *buf = xmalloc(buffsize);
    memset(buf, 0xA5, buffsize);

    snap_t s0, s1;
    long written = 0;
    long ops = 0;
    take_snapshot(&s0);
    while (written < total_bytes) {
        if (max_ops > 0 && ops >= max_ops)   /* O_SYNC 下限制写次数 */
            break;
        size_t n = buffsize;
        if ((long)n > total_bytes - written)
            n = (size_t)(total_bytes - written);
        ssize_t w = write(fd, buf, n);
        if (w < 0)
            die("write 失败");
        written += w;
        ops++;
    }
    take_snapshot(&s1);

    free(buf);
    close(fd);
    *bytes_out = written;
    return compute_timing(&s0, &s1);
}

/* ===================== ③ 结果聚合与输出 ===================== */

static void csv_open(const char *path)
{
    g_csv = fopen(path, "w");
    if (!g_csv)
        die("无法创建 CSV 文件");
    fprintf(g_csv, "test,buffsize,real,user,sys,bytes,throughput_MBps\n");
}

static void csv_row(const char *test, size_t buffsize, timing_t t, long bytes)
{
    if (!g_csv)
        return;
    fprintf(g_csv, "%s,%zu,%.6f,%.6f,%.6f,%ld,%.4f\n",
            test, buffsize, t.real, t.user, t.sys, bytes,
            throughput_mbps(bytes, t.real));
}

static void csv_close(void)
{
    if (g_csv) {
        fclose(g_csv);
        g_csv = NULL;
    }
}

static void print_table_header(const char *title)
{
    printf("\n%s\n %s\n%s\n", SEP, title, SEP);
    printf("%12s %12s %12s %12s %18s\n",
           "BUFFSIZE", "real(s)", "user(s)", "sys(s)", "throughput(MB/s)");
    fflush(stdout);
}

static void print_table_row(const result_t *r)
{
    printf("%12zu %12.4f %12.4f %12.4f %18.2f\n",
           r->buffsize, r->t.real, r->t.user, r->t.sys,
           throughput_mbps(r->bytes, r->t.real));
    fflush(stdout);
}

static void print_single(const char *name, timing_t t, long bytes)
{
    printf("  %-10s real=%9.4fs  user=%9.4fs  sys=%9.4fs  throughput=%8.2f MB/s\n",
           name, t.real, t.user, t.sys, throughput_mbps(bytes, t.real));
    fflush(stdout);
}

/* ===================== 命令行 ===================== */

static void usage(const char *prog)
{
    printf("用法: %s [选项]\n", prog);
    printf("  -f PATH   读测试文件路径           (默认 ./testfile)\n");
    printf("  -s MB     读测试文件大小, MB        (默认 %d)\n", DEF_READ_MB);
    printf("  -w PATH   写测试文件路径           (默认 ./writefile)\n");
    printf("  -W MB     无 O_SYNC 写入总量, MB    (默认 %d)\n", DEF_WRITE_MB);
    printf("  -S MB     O_SYNC 写入总量, MB       (默认 %d)\n", DEF_OSYNC_MB);
    printf("  -o PATH   结果 CSV 输出路径         (默认 ./results.csv)\n");
    printf("  -d        每次读测试前用 posix_fadvise 逐出该文件页缓存\n");
    printf("  -h        显示本帮助并退出\n");
}

/* ===================== main: 串起 ①②③ 流程 ===================== */

int main(int argc, char **argv)
{
    const char *read_path  = "testfile";
    const char *write_path = "writefile";
    const char *csv_path   = "results.csv";
    long read_mb  = DEF_READ_MB;
    long write_mb = DEF_WRITE_MB;
    long osync_mb = DEF_OSYNC_MB;

    int opt;
    while ((opt = getopt(argc, argv, "f:s:w:W:S:o:dh")) != -1) {
        switch (opt) {
        case 'f': read_path  = optarg;      break;
        case 's': read_mb    = atol(optarg); break;
        case 'w': write_path = optarg;      break;
        case 'W': write_mb   = atol(optarg); break;
        case 'S': osync_mb   = atol(optarg); break;
        case 'o': csv_path   = optarg;      break;
        case 'd': g_drop_cache = 1;         break;
        case 'h': usage(argv[0]);           return 0;
        default:  usage(argv[0]);           return 1;
        }
    }
    if (read_mb <= 0 || write_mb <= 0 || osync_mb <= 0) {
        fprintf(stderr, "错误: 文件大小 / 写入总量必须为正数\n");
        return 1;
    }

    /* ---------- ① 确定版本环境 ---------- */
    detect_environment(read_path);

    printf("\n 测试参数\n%s\n", SEP);
    printf("  读测试文件          : %s (%ld MB)\n", read_path, read_mb);
    printf("  写测试文件          : %s\n", write_path);
    printf("  写入总量 (无 O_SYNC) : %ld MB\n", write_mb);
    printf("  写入总量 (O_SYNC)    : %ld MB (每个 BUFFSIZE 最多 %d 次 write)\n",
           osync_mb, OSYNC_MAX_WRITES);
    printf("  CSV 输出            : %s\n", csv_path);
    printf("  测前逐出页缓存       : %s\n", g_drop_cache ? "是 (-d)" : "否");
    if (!g_drop_cache) {
        printf("\n提示: 为获得准确数据, 建议测试前执行清缓存:\n");
        printf("      sync && echo 3 | sudo tee /proc/sys/vm/drop_caches\n");
        printf("      或加 -d 选项让程序在每次读测试前逐出该文件页缓存。\n");
    }

    printf("\n");
    ensure_test_file(read_path, read_mb);

    csv_open(csv_path);

    result_t r;
    long bytes;

    /* ---------- ② 分别调用测试函数 ---------- */

    /* 2.2.1  read() */
    print_table_header("② 2.2.1  read() 系统调用 — 不同 BUFFSIZE");
    for (int i = 0; i < N_BUFFSIZES; i++) {
        r.buffsize = BUFFSIZES[i];
        r.t = test_read(read_path, BUFFSIZES[i], &bytes);
        r.bytes = bytes;
        print_table_row(&r);
        csv_row("read", BUFFSIZES[i], r.t, bytes);
    }

    /* 2.2.2  getc 宏 vs fgetc 函数 */
    printf("\n%s\n ② 2.2.2  stdio 逐字符读取 (getc 宏 vs fgetc 函数)\n%s\n",
           SEP, SEP);
    {
        timing_t t = test_getc(read_path, &bytes);
        print_single("getc()", t, bytes);
        csv_row("getc", 1, t, bytes);

        t = test_fgetc(read_path, &bytes);
        print_single("fgetc()", t, bytes);
        csv_row("fgetc", 1, t, bytes);
    }

    /* 2.2.3  fread() */
    print_table_header("② 2.2.3  fread() 库函数 — 不同 BUFFSIZE");
    for (int i = 0; i < N_BUFFSIZES; i++) {
        r.buffsize = BUFFSIZES[i];
        r.t = test_fread(read_path, BUFFSIZES[i], &bytes);
        r.bytes = bytes;
        print_table_row(&r);
        csv_row("fread", BUFFSIZES[i], r.t, bytes);
    }

    /* 2.2.4  my_fread() */
    print_table_header(
        "② 2.2.4  my_fread() 自实现 (内部缓冲固定 BUFSIZ) — 不同请求大小");
    for (int i = 0; i < N_BUFFSIZES; i++) {
        r.buffsize = BUFFSIZES[i];
        r.t = test_my_fread(read_path, BUFFSIZES[i], &bytes);
        r.bytes = bytes;
        print_table_row(&r);
        csv_row("my_fread", BUFFSIZES[i], r.t, bytes);
    }

    /* 2.2.5  write() 无 O_SYNC */
    print_table_header("② 2.2.5  write() 无 O_SYNC — 不同 BUFFSIZE");
    {
        long wtotal = write_mb * MB;
        for (int i = 0; i < N_BUFFSIZES; i++) {
            r.buffsize = BUFFSIZES[i];
            r.t = test_write(write_path, BUFFSIZES[i], 0, wtotal, 0, &bytes);
            r.bytes = bytes;
            print_table_row(&r);
            csv_row("write_nosync", BUFFSIZES[i], r.t, bytes);
        }
    }

    /* 2.2.6  write() 有 O_SYNC */
    print_table_header("② 2.2.6  write() 有 O_SYNC — 不同 BUFFSIZE");
    printf("(注: O_SYNC 每次写都同步落盘, 各 BUFFSIZE 最多写 %d 次,"
           " 实际写入量见吞吐量与 CSV)\n", OSYNC_MAX_WRITES);
    {
        long stotal = osync_mb * MB;
        for (int i = 0; i < N_BUFFSIZES; i++) {
            r.buffsize = BUFFSIZES[i];
            r.t = test_write(write_path, BUFFSIZES[i], 1, stotal,
                             OSYNC_MAX_WRITES, &bytes);
            r.bytes = bytes;
            print_table_row(&r);
            csv_row("write_osync", BUFFSIZES[i], r.t, bytes);
        }
    }

    /* ---------- ③ 聚合输出 ---------- */
    csv_close();
    unlink(write_path);

    printf("\n%s\n", SEP);
    printf(" ③ 全部测试完成, 结果已汇总写入: %s\n", csv_path);
    printf("%s\n", SEP);
    return 0;
}
