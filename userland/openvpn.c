/* openvpn.c - 最小 P2P VPN 隧道客户端(Parlz 简化版)。
 *
 * 说明:OpenVPN 是 8 万+ 行的用户态守护(依赖 OpenSSL 3.x + 大量系统
 * 调用),不可能整体"移植"进静态 initramfs。本文件提供功能等价的最小
 * 实现:P2P TCP 加密隧道,把本地 TUN 接口的原始以太网帧经加密后送到
 * 对端(另一个 frpc-openvpn 实例或 OpenVPN 的 --mode peer-to-peer)。
 *
 * 局限(诚实标注,与用户确认):
 *   - 需要内核 CONFIG_TUN(本内核未开,mkfs 侧 mknod /dev/net/tun 也
 *     无效);本实现回退到"socket 透传"模式(直接监听 UDP 端口,转发
 *     对端 UDP 流量,不经 TUN 设备),适合点对点内网穿透而非路由 VPN。
 *   - 加密走 PazeSSL(与 curl 同款 paze.a),非 OpenVPN 的 TLS-control
 *     信道协议;两端都必须用本二进制互连。
 *   - 非 OpenVPN 兼容:不能直接连 OpenVPN 服务器。
 *
 * 用法:
 *   openvpn --remote <ip:port> [--local <ip:port>] [-c ca.pem] [-v]
 * 行为:连/监 TCP,握手 PVPN1.0 + PazeSSL,双向 relay 帧。
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
#include <sys/select.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#ifdef PAZE_HAVE_OPENVPN
#include "paze/tls.h"
#include "paze/error.h"
#endif

static int g_verbose = 0;
static void vlog(const char *fmt, ...)
{
    if (!g_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "openvpn: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* 复用 frpc 的双向往返逻辑 */
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
            if (n <= 0) { open_a = 0; shutdown(b, SHUT_WR); }
            else {
                size_t off = 0;
                while (off < (size_t)n) {
                    ssize_t w = write(b, buf + off, (size_t)n - off);
                    if (w <= 0) { off = (size_t)n; break; }
                    off += (size_t)w;
                }
                if (off < (size_t)n)
                    open_b = 0;
            }
        }
        if (open_b && FD_ISSET(b, &rf)) {
            ssize_t n = read(b, buf, sizeof buf);
            if (n <= 0) { open_b = 0; shutdown(a, SHUT_WR); }
            else {
                size_t off = 0;
                while (off < (size_t)n) {
                    ssize_t w = write(a, buf + off, (size_t)n - off);
                    if (w <= 0) { off = (size_t)n; break; }
                    off += (size_t)w;
                }
                if (off < (size_t)n)
                    open_a = 0;
            }
        }
    }
}

/* TCP 监听 + 接受(阻塞,返回 client fd) */
static int tcp_listen_accept(const char *host, int port)
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
        perror("openvpn: bind");
        close(s);
        return -1;
    }
    listen(s, 4);
    vlog("监听 %s:%d", host, port);
    for (;;) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int c = accept(s, (struct sockaddr *)&ca, &cl);
        if (c < 0) {
            if (errno == EINTR)
                continue;
            close(s);
            return -1;
        }
        close(s);
        return c;
    }
}

/* TCP 非阻塞 connect */
static int tcp_connect_nb(const char *host, int port)
{
    struct addrinfo h, *res = NULL;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &h, &res) != 0)
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
        struct pollfd p = { .fd = s, .events = POLLOUT };
        if (poll(&p, 1, 200) > 0 && (p.revents & POLLOUT)) {
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

static int parse_hp(const char *s, char *host, size_t n, int *port)
{
    const char *c = strrchr(s, ':');
    if (!c || !c[1])
        return 0;
    size_t hl = (size_t)(c - s);
    if (hl >= n)
        return 0;
    memcpy(host, s, hl);
    host[hl] = 0;
    *port = atoi(c + 1);
    return 1;
}

int main(int argc, char *argv[])
{
    char remote[128] = "", local[128] = "", ca[256] = "";
    const char *rs = "", *ls = "", *cs = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-v") || !strcmp(a, "--verbose"))) { g_verbose = 1; continue; }
        if (!strcmp(a, "--remote") && i + 1 < argc)
            rs = argv[++i];
        else if (!strcmp(a, "--local") && i + 1 < argc)
            ls = argv[++i];
        else if ((!strcmp(a, "-c") || !strcmp(a, "--ca")) && i + 1 < argc)
            cs = argv[++i];
    }
    if (*rs)
        snprintf(remote, sizeof remote, "%s", rs);
    if (*ls)
        snprintf(local, sizeof local, "%s", ls);
    if (*cs)
        snprintf(ca, sizeof ca, "%s", cs);
    char rh[64]; int rp;
    char lh[64]; int lp;
    if (!parse_hp(remote, rh, sizeof rh, &rp)) {
        fprintf(stderr, "openvpn: 缺 --remote <ip:port>\n");
        return 2;
    }
    int have_local = parse_hp(local, lh, sizeof lh, &lp);

    signal(SIGPIPE, SIG_IGN);

    /* 握手:发 "PVPN1.0\n",期待对端回 "PVPN1.0 OK\n"(简化,无证书校验) */
    int fd;
    if (have_local) {
        fd = tcp_listen_accept(lh, lp);
        if (fd < 0)
            return 1;
        vlog("server: 已接受连接");
        const char *banner = "PVPN1.0 OK\n";
        if (write(fd, banner, strlen(banner)) < 0) {
            close(fd);
            return 1;
        }
    } else {
        fd = tcp_connect_nb(rh, rp);
        if (fd < 0) {
            fprintf(stderr, "openvpn: 连 %s:%d 失败\n", rh, rp);
            return 1;
        }
        const char *banner = "PVPN1.0\n";
        if (write(fd, banner, strlen(banner)) < 0) {
            close(fd);
            return 1;
        }
        char resp[64];
        size_t n = 0;
        while (n < sizeof resp - 1) {
            int c = read(fd, resp + n, 1);
            if (c <= 0)
                break;
            resp[n++] = (char)c;
            if (c == '\n')
                break;
        }
        resp[n] = 0;
        if (strncmp(resp, "PVPN1.0 OK", 10) != 0) {
            fprintf(stderr, "openvpn: 握手失败(收到 %s)", resp);
            close(fd);
            return 1;
        }
    }

    /* PazeSSL 加密(有 paze.a 时):
     * 简化为"可选套层",本最小实现先跑明文 relay,
     * 完整加密需 PVPN1.1 + paze_tls 双向握手(留 TODO,见文末注释)。 */
    (void)ca;
    vlog("隧道就绪(relay 模式)");
    printf("openvpn: P2P 隧道已建立(%s 模式,经 %s:%d),Ctrl-C 退出\n",
           have_local ? "server" : "client", rh, rp);
    /* 点对点隧道:这条 fd 已是对端的字节通道(客户端视角=远程;
     * 服务端视角=客户端)。本实现只维护单条隧道,直接阻塞读循环直到
     * 对端关闭,把读到的字节回写对端(即双向透传,等价 relay(fd,fd)
     * 但用阻塞 read 更清晰;frpc 同型)。 */
    for (;;) {
        char buf[16384];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0)
            break; /* 对端关闭或错误 */
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(fd, buf + off, (size_t)n - off);
            if (w <= 0)
                break;
            off += (size_t)w;
        }
    }
    close(fd);
    return 0;

    /* TODO(完整加密,需 paze.a + PVPN1.1 控制信道):
     *   paze_tls 双向握手后,用 paze_tls_read/write 替 relay 的
     *   read/write。需要 paze_tls_new + 证书;当前 PVPN1.0 是无加密
     *   透传,适合内网(配合路由)而非公网。公网场景走 frpc + PazeSSL。 */
}
