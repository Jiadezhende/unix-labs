# 系统程序设计 Lab 1 — Part B 构建脚本
#
# 服务器/客户端各自为独立可执行文件, 均链接 src/common.c。
# 随实验阶段推进, 把新增源文件名加入 SERVERS / CLIENTS 即可。

CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
LDLIBS  = -lpthread

SRCDIR  = src
BINDIR  = bin

# 服务器: poll / 线程池 / epoll (epoll 为加分项)
SERVERS = server_poll server_pool server_epoll
# 压测客户端
CLIENTS = client

TARGETS = $(addprefix $(BINDIR)/,$(SERVERS) $(CLIENTS))

COMMON_SRC = $(SRCDIR)/common.c
COMMON_HDR = $(SRCDIR)/common.h

all: $(TARGETS)

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/%: $(SRCDIR)/%.c $(COMMON_SRC) $(COMMON_HDR) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $< $(COMMON_SRC) $(LDLIBS)

clean:
	rm -rf $(BINDIR)

.PHONY: all clean
