/* frpc.c - FRPC (Parlz FRP Client):TCP 端口转发最小实现。
 *
 * 与 frp(fanuang/frp)的 frpc 协议不完全兼容,而是 Paze 风格的简化版:
 * 通过 TCP 连接远端 frps,先发一行 "TUNNEL <local:port> <remote:port>\n",
 * 后续字节直通双向转发(本地客户端 <-> frps 远端服务)。
 *
 * 用法:
 *   frpc --server <ip:port> --local <ip:port> --remote <ip:port>
 *   frpc -s 1.2.3.4:7000 -l 127.0.0.1:80 -r 0.0.0.0:8080
 *
 * 行为:
 *   1. 连 frps(server);
 *   2. 发 TUNNEL 行,frps 回 "OK\n" 或 "ERR <reason>\n";
 *   3. 本地监听 local,每来一个连接,经 frps 通道转发到 remote。
 * 单向控制,无加密(加密走 PazeSSL/SSH 层,frpc 只做纯 TCP 隧道)。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>

static int g_verbose = 0;

static void vlog(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "frpc: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* 解析 "host:port" */
static int parse_hostport(const char *s, char *host, size_t hostsz, int *port)
{
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s || !colon[1])
        return 0;
    size_t hl = (size_t)(colon - s);
    if (hl >= hostsz)
        return 0;
    memcpy(host, s, hl);
    host[hl] = 0;
    *port = atoi(colon + 1);
    return 1;
}

/* 非阻塞 connect + poll(超时 ~10s) */
static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0)
        return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    int ok = 0;
    for (int i = 0; i < 50; i++) {
        int r = connect(s, res->ai_addr, res->ai_addrlen);
        if (r == 0) { ok = 1; break; }
        if (errno != EINPROGRESS && errno != EAGAIN && errno != EINTR)
            break;
        struct pollfd pfd = { .fd = s, .events = POLLOUT };
        if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLOUT)) {
            int err = 0, el = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            ok = (err == 0);
            break;
        }
    }
    fcntl(s, F_SETFL, flags);
    freeaddrinfo(res);
    if (!ok) {
        close(s);
        return -1;
    }
    return s;
}

/* 监听 local:port,返回 listen fd */
static int listen_local(const char *host, int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    if (!strcmp(host, "0.0.0.0") || !strcmp(host, "*"))
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        perror("frpc: bind local");
        close(s);
        return -1;
    }
    if (listen(s, 16) < 0) {
        perror("frpc: listen");
        close(s);
        return -1;
    }
    return s;
}

/* 双向往返:从 a 读到就写 b,直到一侧 EOF。用于本地<->frps 通道 */
static void relay(int a, int b)
{
    char buf[16384];
    int open_a = 1, open_b = 1;
    while (open_a || open_b) {
        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = 0;
        if (open_a) { FD_SET(a, &rf); if (a > maxfd) maxfd = a; }
        if (open_b) { FD_SET(b, &rf); if (b > maxfd) maxfd = b; }
        if (maxfd < 0)
            break;
        if (select(maxfd + 1, &rf, NULL, NULL, NULL) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (open_a && FD_ISSET(a, &rf)) {
            ssize_t n = read(a, buf, sizeof buf);
            if (n <= 0) {
                open_a = 0;
                shutdown(b, SHUT_WR); /* 告诉 b 读端完成 */
            } else {
                size_t off = 0;
                while (off < (size_t)n) {
                    ssize_t w = write(b, buf + off, (size_t)n - off);
                    if (w <= 0) {
                        off = (size_t)n; /* 对端关闭/错误,停止 */
                        break;
                    }
                    off += (size_t)w;
                }
                if (off < (size_t)n)
                    open_b = 0;
            }
        }
        if (open_b && FD_ISSET(b, &rf)) {
            ssize_t n = read(b, buf, sizeof buf);
            if (n <= 0) {
                open_b = 0;
                shutdown(a, SHUT_WR);
            } else {
                size_t off = 0;
                while (off < (size_t)n) {
                    ssize_t w = write(a, buf + off, (size_t)n - off);
                    if (w <= 0) {
                        off = (size_t)n;
                        break;
                    }
                    off += (size_t)w;
                }
                if (off < (size_t)n)
                    open_a = 0;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    char srv[128] = "", loc[128] = "", rem[128] = "";
    const char *ss = "", *ls = "", *rs = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { g_verbose = 1; continue; }
        if ((!strcmp(a, "-s") || !strcmp(a, "--server")) && i + 1 < argc)
            ss = argv[++i];
        else if ((!strcmp(a, "-l") || !strcmp(a, "--local")) && i + 1 < argc)
            ls = argv[++i];
        else if ((!strcmp(a, "-r") || !strcmp(a, "--remote")) && i + 1 < argc)
            rs = argv[++i];
    }
    if (*ss)
        snprintf(srv, sizeof srv, "%s", ss);
    if (*ls)
        snprintf(loc, sizeof loc, "%s", ls);
    if (*rs)
        snprintf(rem, sizeof rem, "%s", rs);
    char sh[64]; int sp;
    /* 解析 local host */
    char lhost[64], rhost[64];
    int lport, rport;
    if (!parse_hostport(srv, sh, sizeof sh, &sp)) {
        fprintf(stderr, "frpc: 缺 --server <ip:port>\n");
        return 2;
    }
    if (!parse_hostport(loc, lhost, sizeof lhost, &lport)) {
        fprintf(stderr, "frpc: 缺 --local <ip:port>\n");
        return 2;
    }
    if (!parse_hostport(rem, rhost, sizeof rhost, &rport)) {
        fprintf(stderr, "frpc: 缺 --remote <ip:port>\n");
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);

    /* 1. 连 frps */
    int frps = tcp_connect(sh, sp);
    if (frps < 0) {
        fprintf(stderr, "frpc: 连 frps %s:%d 失败\n", sh, sp);
        return 1;
    }
    vlog("已连 frps %s:%d", sh, sp);

    /* 2. 发 TUNNEL 行:local:port remote:host:port */
    char cmd[256];
    int cl = snprintf(cmd, sizeof cmd, "TUNNEL %s:%d %s:%d\n",
                      lhost, lport, rhost, rport);
    if (write(frps, cmd, (size_t)cl) != cl) {
        close(frps);
        fprintf(stderr, "frpc: 发 TUNNEL 失败\n");
        return 1;
    }
    vlog("TUNNEL %s -> %s", loc, rem);
    /* 读一行响应 */
    char resp[128];
    size_t rl = 0;
    int c;
    while (rl < sizeof resp - 1 && (c = read(frps, resp + rl, 1)) == 1) {
        resp[rl++] = (char)c;
        if (c == '\n')
            break;
    }
    resp[rl] = 0;
    if (strncmp(resp, "OK", 2) != 0) {
        fprintf(stderr, "frpc: frps 拒绝: %s", resp);
        close(frps);
        return 1;
    }

    /* 3. 本地监听 + 每连接转发 */
    int lfd = listen_local(lhost, lport);
    if (lfd < 0) {
        close(frps);
        return 1;
    }
    vlog("监听 %s:%d", lhost, lport);
    printf("frpc: 隧道就绪 %s <-> %s:%d (经 %s:%d), Ctrl-C 退出\n",
           loc, rhost, rport, sh, sp);

    for (;;) {
        struct sockaddr_in ca;
        socklen_t cl2 = sizeof ca;
        int cli = accept(lfd, (struct sockaddr *)&ca, &cl2);
        if (cli < 0)
            continue;
        /* 把 frps 与本地客户端双向 relay(复用同一 frps 连接,
         * 简化为顺序处理:本实现同一时刻服务一个客户端) */
        vlog("客户端接入");
        relay(cli, frps);
        close(cli);
        /* 简化:relay 结束意味着会话结束,重建 frps 连接 */
        close(frps);
        frps = tcp_connect(sh, sp);
        if (frps < 0) {
            fprintf(stderr, "frpc: 重连 frps 失败,退出\n");
            break;
        }
        if (write(frps, cmd, (size_t)cl) != cl)
            break;
    }
    close(lfd);
    return 0;
}
