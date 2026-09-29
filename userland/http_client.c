/* Parlz HTTP/HTTPS 共享引擎。固定大小缓冲、单请求、总期限、失败关闭。 */
#define _GNU_SOURCE 1
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef PARLZ_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#define URL_MAX 8192
#define LINE_MAXIMUM 8192
#define HEADER_MAXIMUM 65536
#define REDIRECT_MAXIMUM 10
struct url { char host[256], authority[272], path[URL_MAX]; int tls, port; };
struct conn {
    int fd;
    int64_t deadline;
    unsigned char buf[16384];
    size_t pos, len;
#ifdef PARLZ_HAVE_OPENSSL
    SSL *ssl;
    SSL_CTX *ctx;
#endif
};
static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int wait_fd(int fd, short events, int64_t deadline) {
    struct pollfd p = {fd, events, 0};
    for (;;) {
        int64_t left = deadline - now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        int n = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
        if (n > 0) return 0;
        if (!n) { errno = ETIMEDOUT; return -1; }
        if (errno != EINTR) return -1;
    }
}
static int error(struct http_result *r, int code, const char *text) {
    snprintf(r->error, sizeof r->error, "%s", text);
    return code;
}
static int copy_text(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) return -1;
    memcpy(dst, src, n); dst[n] = 0; return 0;
}
static int parse_url(const char *text, struct url *u) {
    const char *p = text, *end, *port = NULL;
    memset(u, 0, sizeof *u);
    if (strlen(text) >= URL_MAX) return -1;
    for (const unsigned char *s = (const unsigned char *)text; *s; s++)
        if (*s <= 32 || *s == 127 || *s == '\\') return -1;
    if (!strncasecmp(p, "https://", 8)) { u->tls = 1; p += 8; }
    else if (!strncasecmp(p, "http://", 7)) p += 7;
    else return -1;
    end = p + strcspn(p, "/?#");
    if (end == p || memchr(p, '@', end-p) || copy_text(u->authority, sizeof u->authority, p, end-p)) return -1;
    if (*p == '[') {
        const char *q = memchr(p, ']', end-p);
        if (!q || copy_text(u->host, sizeof u->host, p+1, q-p-1)) return -1;
        if (q+1 != end) { if (q[1] != ':') return -1; port = q+2; }
    } else {
        const char *q = memchr(p, ':', end-p);
        if (copy_text(u->host, sizeof u->host, p, q ? q-p : end-p)) return -1;
        if (q) port = q+1;
    }
    if (!u->host[0]) return -1;
    u->port = u->tls ? 443 : 80;
    if (port) {
        unsigned value = 0;
        if (port == end) return -1;
        for (; port < end; port++) {
            if (*port < '0' || *port > '9' || value > 6553) return -1;
            value = value*10 + *port-'0';
        }
        if (!value || value > 65535) return -1;
        u->port = value;
    }
    size_t n = strcspn(end, "#");
    if (*end == '/') return copy_text(u->path, sizeof u->path, end, n);
    u->path[0] = '/';
    return copy_text(u->path+1, sizeof u->path-1, end, n);
}

/* 在子进程中调用系统解析器，避免 getaddrinfo 超出总期限；不自写 DNS 解析。 */
struct address { socklen_t len; struct sockaddr_storage sa; };
static int connect_host(struct conn *c, const struct url *u) {
    int pipes[2], result = -1;
    if (pipe(pipes)) return -1;
    pid_t pid = fork();
    if (pid == -1) { close(pipes[0]); close(pipes[1]); return -1; }
    if (!pid) {
        struct addrinfo hints = {0}, *list = NULL;
        char service[8];
        close(pipes[0]);
        hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
        snprintf(service, sizeof service, "%d", u->port);
        if (!getaddrinfo(u->host, service, &hints, &list)) {
            int count = 0;
            for (struct addrinfo *a = list; a && count < 16; a = a->ai_next) {
                struct address v = {0};
                if (a->ai_addrlen > sizeof v.sa) continue;
                v.len = a->ai_addrlen; memcpy(&v.sa, a->ai_addr, v.len);
                if (write(pipes[1], &v, sizeof v) != sizeof v) break;
                count++;
            }
            freeaddrinfo(list);
        }
        close(pipes[1]); _exit(0);
    }
    close(pipes[1]);
    for (;;) {
        struct address v;
        size_t have = 0;
        while (have < sizeof v) {
            if (wait_fd(pipes[0], POLLIN, c->deadline)) goto done;
            ssize_t n = read(pipes[0], (char *)&v+have, sizeof v-have);
            if (n <= 0) goto done;
            have += n;
        }
        int fd = socket(v.sa.ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) continue;
        int rc = connect(fd, (struct sockaddr *)&v.sa, v.len);
        if (rc && errno == EINPROGRESS && !wait_fd(fd, POLLOUT, c->deadline)) {
            int e = 0; socklen_t len = sizeof e;
            if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len)) { errno = e; rc = e ? -1 : 0; }
        }
        if (!rc) { c->fd = fd; result = 0; break; }
        close(fd);
    }
done:
    { int saved = errno; close(pipes[0]); kill(pid, SIGKILL);
      while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
      errno = saved; }
    return result;
}
static int socket_read(void *ctx, uint8_t *buf, size_t len) {
    struct conn *c = ctx;
    for (;;) {
        if (wait_fd(c->fd, POLLIN, c->deadline)) return -1;
        ssize_t n = recv(c->fd, buf, len, 0);
        if (n >= 0) return (int)n;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
}
static int socket_write(void *ctx, const uint8_t *buf, size_t len) {
    struct conn *c = ctx;
    size_t sent = 0;
    while (sent < len) {
        if (wait_fd(c->fd, POLLOUT, c->deadline)) return -1;
        ssize_t n = send(c->fd, buf+sent, len-sent, MSG_NOSIGNAL);
        if (n > 0) sent += n;
        else if (!n || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return -1;
    }
    return (int)sent;
}
static void disconnect(struct conn *c) {
#ifdef PARLZ_HAVE_OPENSSL
    /* SSL_shutdown 依赖已关闭的 fd 时无害；先关 TLS 再关 socket。 */
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ctx) {
        SSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
#endif
    if (c->fd >= 0) close(c->fd);
    c->fd = -1; c->pos = c->len = 0;
}
static int start_tls(struct conn *c, const struct url *u, struct http_result *r,
                     const char *cacert, int insecure) {
#ifdef PARLZ_HAVE_OPENSSL
    struct in_addr addr;
    int is_ip = inet_pton(AF_INET, u->host, &addr) == 1;
    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) return error(r, 35, "SSL_CTX 分配失败");
    /* 跟随服务器协商 1.2/1.3；容忍不发 close_notify 的服务器(视为正常 EOF)。 */
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_VERSION);
    SSL_CTX_set_options(c->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
    /* 主机名/IP 校验目标：证书含 IP SAN 时按 IP 匹配，否则按 DNS 匹配。 */
    X509_VERIFY_PARAM *param = SSL_CTX_get0_param(c->ctx);
    if (is_ip) {
        if (!X509_VERIFY_PARAM_set1_ip(param, (const unsigned char *)&addr.s_addr, sizeof addr.s_addr))
            return error(r, 35, "IP 校验设置失败");
    } else if (!X509_VERIFY_PARAM_set1_host(param, u->host, 0)) {
        return error(r, 35, "主机名校验设置失败");
    }
    if (cacert && !SSL_CTX_load_verify_locations(c->ctx, cacert, NULL))
        return error(r, 58, "无法加载 CA 文件");
    if (insecure)
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);
    else {
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
        /* 系统信任库:initramfs 内置于 /etc/ssl/cert.pem(编译期 --openssldir=/etc/ssl)。 */
        SSL_CTX_set_default_verify_paths(c->ctx);
    }
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) return error(r, 35, "SSL 分配失败");
    if (!SSL_set_fd(c->ssl, c->fd)) return error(r, 35, "SSL 绑定 fd 失败");
    if (!is_ip && SSL_set_tlsext_host_name(c->ssl, u->host) != 1)
        return error(r, 35, "SNI 设置失败");
    ERR_clear_error();
    for (;;) {
        int rc = SSL_connect(c->ssl);
        if (rc == 1) break;
        int err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_WANT_READ && !wait_fd(c->fd, POLLIN, c->deadline)) continue;
        if (err == SSL_ERROR_WANT_WRITE && !wait_fd(c->fd, POLLOUT, c->deadline)) continue;
        char buf[160];
        unsigned long e = ERR_peek_last_error();
        ERR_error_string_n(e, buf, sizeof buf);
        snprintf(r->error, sizeof r->error, "TLS 握手失败: %s", e ? buf : "socket 或超时");
        return 60;
    }
    return 0;
#else
    (void)c; (void)u; (void)cacert; (void)insecure;
    return error(r, 35, "此构建未链接 OpenSSL，HTTPS 不可用");
#endif
}
static int fill(struct conn *c) {
    if (now_ms() >= c->deadline) { errno = ETIMEDOUT; return -1; }
    int n;
#ifdef PARLZ_HAVE_OPENSSL
    if (c->ssl) {
        /* WANT_READ/WRITE 是可重试状态,必须循环到拿到数据/EOF/错误;
         * 返回 0 会被上层当成 EOF,把分片到达的响应头误判为截断。 */
        for (;;) {
            ERR_clear_error();
            n = SSL_read(c->ssl, c->buf, (int)sizeof c->buf);
            if (n > 0) break;
            int err = SSL_get_error(c->ssl, n);
            if (err == SSL_ERROR_WANT_READ) {
                if (wait_fd(c->fd, POLLIN, c->deadline)) return -1;
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                if (wait_fd(c->fd, POLLOUT, c->deadline)) return -1;
                continue;
            }
            /* ZERO_RETURN = close_notify;SYSCALL+errno==0 = 对端直接关闭
             * (服务器不发 close_notify 时 OpenSSL 3.x 归为 unexpected EOF)。 */
            if (err == SSL_ERROR_ZERO_RETURN) return 0;
            if (err == SSL_ERROR_SYSCALL && !errno) return 0;
            return -1;
        }
    } else
#endif
    n = socket_read(c, c->buf, sizeof c->buf);
    if (n > 0) { c->pos = 0; c->len = (size_t)n; }
    return n;
}
static int byte(struct conn *c) {
    if (c->pos == c->len) { int n = fill(c); if (n <= 0) return -1; }
    return c->buf[c->pos++];
}
static int line(struct conn *c, char *s, size_t cap, size_t *budget) {
    size_t n = 0;
    for (;;) {
        int b = byte(c);
        if (b < 0 || !*budget) return -1;
        --*budget;
        if (b == '\r') {
            if (!*budget || byte(c) != '\n') return -1;
            --*budget; s[n] = 0; return 0;
        }
        if (!b || b == '\n' || (b < 32 && b != '\t') || b == 127 || n+1 >= cap) return -1;
        s[n++] = (char)b;
    }
}
static int number(const char *s, unsigned base, uint64_t *out, const char **end) {
    uint64_t n = 0; const char *p = s;
    for (; *p; p++) {
        unsigned d = *p >= '0' && *p <= '9' ? (unsigned)(*p-'0') :
            *p >= 'a' && *p <= 'f' ? (unsigned)(*p-'a'+10) :
            *p >= 'A' && *p <= 'F' ? (unsigned)(*p-'A'+10) : 99;
        if (d >= base) break;
        if (n > (UINT64_MAX-d)/base) return -1;
        n = n*base+d;
    }
    if (p == s) return -1;
    *out = n; *end = p; return 0;
}
static char *field(char *s) {
    char *colon = strchr(s, ':');
    if (!colon || colon == s) return NULL;
    for (char *p = s; p < colon; p++)
        if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p)) return NULL;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t') colon++;
    char *end = colon+strlen(colon);
    while (end > colon && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return colon;
}
struct response { int status, has_length, chunked; uint64_t length; char location[URL_MAX]; };
static int headers(struct conn *c, struct response *h) {
    char s[LINE_MAXIMUM]; size_t budget = HEADER_MAXIMUM;
    for (int interim = 0; interim < 9; interim++) {
        memset(h, 0, sizeof *h);
        if (line(c, s, sizeof s, &budget) || strlen(s) < 12 ||
            (strncmp(s, "HTTP/1.1 ", 9) && strncmp(s, "HTTP/1.0 ", 9)) ||
            !isdigit((unsigned char)s[9]) || !isdigit((unsigned char)s[10]) ||
            !isdigit((unsigned char)s[11]) || (s[12] && s[12] != ' ')) return -1;
        h->status = (s[9]-'0')*100+(s[10]-'0')*10+s[11]-'0';
        if (h->status < 100 || h->status > 599) return -1;
        for (;;) {
            if (line(c, s, sizeof s, &budget)) return -1;
            if (!*s) break;
            char *v = field(s); if (!v) return -1;
            if (!strcasecmp(s, "Content-Length")) {
                uint64_t n; const char *end;
                if (number(v, 10, &n, &end) || *end || (h->has_length && h->length != n)) return -1;
                h->has_length = 1; h->length = n;
            } else if (!strcasecmp(s, "Transfer-Encoding")) {
                if (h->chunked || strcasecmp(v, "chunked")) return -1;
                h->chunked = 1;
            } else if (!strcasecmp(s, "Location")) {
                if (h->location[0] || copy_text(h->location, sizeof h->location, v, strlen(v))) return -1;
            } else if (!strcasecmp(s, "Content-Encoding") && strcasecmp(v, "identity")) return -1;
        }
        if (h->has_length && h->chunked) return -1;
        if (h->status >= 200) return 0;
        if (h->status == 101 || h->has_length || h->chunked) return -1;
    }
    return -1;
}
/* 下载进度上下文:由 http_cli 填充,回调时渲染。
 * wget:60 格进度条 + 百分比 + 已下载/总量 + 速率(GNU wget -b - 风格);
 * curl:50 格 curl -# 风格(####...#...> 100%)。
 * 回调写 stderr,不污染正文输出;完成后停住不再刷新。 */
struct progress_ctx {
    int wget, quiet, done;
    uint64_t got, total;
    int last_bar, has_rate;
    int64_t start_ms, last_ms;
    uint64_t last_bytes;
    double last_rate;
};
static void progress_render(struct progress_ctx *ctx, FILE *fp) {
    uint64_t got = ctx->got, total = ctx->total;
    int p = total ? (int)(got * 100 / total) : 0;
    if (p > 100) p = 100;
    int bar = total ? p * 60 / 100 : 0;
    if (bar < ctx->last_bar) bar = ctx->last_bar;
    ctx->last_bar = bar;
    /* 速率:最近 250ms 窗口内的平均字节/秒 */
    int64_t now = now_ms();
    if (!ctx->has_rate)
        ctx->start_ms = ctx->last_ms = now;
    int64_t dt = now - ctx->last_ms;
    double rate = ctx->last_rate;
    if (dt >= 250) {
        double dts = (double)dt / 1000.0;
        rate = (double)(got - ctx->last_bytes) / dts;
        ctx->last_rate = rate;
        ctx->last_ms = now;
        ctx->last_bytes = got;
    }
    fputc('\r', fp);
    if (ctx->wget) {
        /* GNU wget 风格:空格填充 + # 填充 + 百分比 + got/total + 速率 */
        for (int i = 0; i < 60; i++)
            fputc(i < bar ? '#' : ' ', fp);
        fprintf(fp, " %3d%%", p);
        if (total)
            fprintf(fp, "  %llu/%llu",
                    (unsigned long long)got, (unsigned long long)total);
        else
            fprintf(fp, "  %lluK", (unsigned long long)(got / 1024));
        if (rate > 0)
            fprintf(fp, "  %5.1fK/s", rate / 1024.0);
    } else {
        /* curl -# 风格:####...#...> 100% */
        int cb = p / 2;
        if (cb > 50) cb = 50;
        for (int i = 0; i < 50; i++) fputc(i < cb ? '#' : '.', fp);
        fputc('>', fp);
        fprintf(fp, " %d%%", p);
    }
    fflush(fp);
}
static void progress_cb(uint64_t got, uint64_t total, void *ud) {
    struct progress_ctx *ctx = ud;
    if (ctx->quiet || ctx->done)
        return;
    ctx->got = got;
    if (ctx->total != total) {
        ctx->total = total;
        ctx->last_bar = 0;
        ctx->has_rate = 0;
    }
    progress_render(ctx, stderr);
    if (total && got >= total) {
        ctx->done = 1;
        /* 完成尾行:wget 打印总字节/速率/耗时;curl 只补换行 */
        if (ctx->wget) {
            int64_t secs = (now_ms() - ctx->start_ms) / 1000;
            double avg = secs > 0 ? (double)got / secs : 0;
            fprintf(stderr, "\r%*s%*s 100%%\n"
                            "wget: %llu 字节, 用时 %llds, 平均 %.1fK/s\n",
                            60, "", 64, "",
                            (unsigned long long)got, (long long)secs, avg / 1024.0);
        } else {
            fputc('\n', stderr);
        }
        fflush(stderr);
    }
}
/* 进度通知:每 16 KiB 块或每个新百分点调一次 pg->cb(节流,避免刷屏) */
static void progress_notify(const struct http_progress *pg, uint64_t got,
                           uint64_t total) {
    if (!pg || !pg->cb)
        return;
    static uint64_t s_got, s_total, s_pct;
    if (total && total != s_total)
        s_total = total, s_got = 0, s_pct = 0;
    int fire = (got - s_got >= 16384) || (got == total);
    if (total) {
        uint64_t p = got * 100 / total;
        if (p > s_pct) { fire = 1; s_pct = p; }
    }
    if (fire) {
        s_got = got;
        pg->cb(got, total, pg->ud);
    }
}

static int body_bytes(struct conn *c, FILE *out, uint64_t count, int until_eof,
                      struct http_result *r, const struct http_progress *pg,
                      uint64_t total) {
    while (until_eof || count) {
        if (c->pos == c->len) {
            int n = fill(c);
            if (!n && until_eof) return 0;
            if (n <= 0) return error(r, 18, "响应体截断或读取失败");
        }
        size_t n = c->len-c->pos;
        if (!until_eof && count < n) n = (size_t)count;
        if (r->bytes > UINT64_MAX-n) return error(r, 63, "响应体过大");
        if (fwrite(c->buf+c->pos, 1, n, out) != n) return error(r, 23, "输出写入失败");
        c->pos += n; r->bytes += n;
        if (!until_eof) count -= n;
        progress_notify(pg, r->bytes, total);
    }
    return 0;
}
static int body(struct conn *c, struct response *h, FILE *out, struct http_result *r,
                const struct http_progress *pg) {
    if (h->status == 204 || h->status == 304) return 0;
    /* 非 chunked:total = Content-Length(已知时)或 0(读到 EOF 为止)。
     * chunked:单个 chunk 的 total 传 0(无 Content-Length),进度按字节数。 */
    uint64_t total = h->has_length ? h->length : 0;
    if (!h->chunked) return body_bytes(c, out, h->length, !h->has_length, r, pg, total);
    for (;;) {
        char s[LINE_MAXIMUM]; size_t budget = sizeof s; uint64_t n; const char *end;
        if (line(c, s, sizeof s, &budget) || number(s, 16, &n, &end) || (*end && *end != ';'))
            return error(r, 18, "无效的 chunk 大小");
        if (!n) {
            budget = HEADER_MAXIMUM;
            for (;;) {
                if (line(c, s, sizeof s, &budget)) return error(r, 18, "trailer 截断或过大");
                if (!*s) return 0;
                if (!field(s) || !strcasecmp(s, "Content-Length") || !strcasecmp(s, "Transfer-Encoding"))
                    return error(r, 18, "无效的 trailer");
            }
        }
        int rc = body_bytes(c, out, n, 0, r, pg, 0); if (rc) return rc;
        if (byte(c) != '\r' || byte(c) != '\n') return error(r, 18, "chunk 缺少 CRLF");
    }
}
static int redirect_url(char *dst, size_t cap, const struct url *u, const char *loc) {
    int n;
    if (strstr(loc, "://")) n = snprintf(dst, cap, "%s", loc);
    else if (!strncmp(loc, "//", 2)) n = snprintf(dst, cap, "%s:%s", u->tls ? "https" : "http", loc);
    else {
        char path[URL_MAX];
        if (*loc == '/') n = snprintf(path, sizeof path, "%s", loc);
        else {
            if (copy_text(path, sizeof path, u->path, strcspn(u->path, "?"))) return -1;
            if (*loc != '?' && *loc != '#') {
                char *last = strrchr(path, '/'); if (last) last[1] = 0;
            }
            size_t len = strlen(path);
            n = snprintf(path+len, sizeof path-len, "%s", loc);
            if (n < 0 || (size_t)n >= sizeof path-len) return -1;
            n = (int)strlen(path);
        }
        if (n < 0 || (size_t)n >= sizeof path) return -1;
        n = snprintf(dst, cap, "%s://%s%s", u->tls ? "https" : "http", u->authority, path);
    }
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
int http_download(const struct http_options *o, struct http_result *r,
                  const struct http_progress *pg) {
    struct conn c = {.fd = -1};
    char current[URL_MAX], seen[REDIRECT_MAXIMUM+1][URL_MAX];
    char *temp = NULL; FILE *out = NULL; int rc = 0;
    memset(r, 0, sizeof *r);
    if (o->timeout < 1 || o->timeout > 86400) return error(r, 2, "超时必须为 1..86400 秒");
    c.deadline = now_ms()+(int64_t)o->timeout*1000;
    int n = snprintf(current, sizeof current, "%s%s", strstr(o->url, "://") ? "" : "http://", o->url);
    if (n < 0 || (size_t)n >= sizeof current) return error(r, 3, "URL 过长");
    for (int hop = 0; ; hop++) {
        struct url u; struct response h;
        if (parse_url(current, &u)) { rc = error(r, 3, "无效或不支持的 URL"); break; }
        for (int i = 0; i < hop; i++) if (!strcmp(seen[i], current)) { rc = error(r, 47, "重定向循环"); goto done; }
        strcpy(seen[hop], current);
        /* OpenSSL 后端有完整 X509 链/主机名/IP 校验：
         * 默认严格校验，--cacert 指定信任源，仅 --insecure 跳过。 */
        errno = 0;
        if (connect_host(&c, &u)) { rc = error(r, 7, "DNS 或连接失败"); break; }
        if (u.tls && (rc = start_tls(&c, &u, r, o->cacert, o->insecure))) break;
        char request[URL_MAX+512];
        n = snprintf(request, sizeof request, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: Parlz-download/1 (OpenSSL)\r\nAccept: */*\r\nAccept-Encoding: identity\r\n\r\n", u.path, u.authority);
        if (n < 0 || (size_t)n >= sizeof request) { rc = error(r, 3, "请求过长"); break; }
        int sent;
#ifdef PARLZ_HAVE_OPENSSL
        if (c.ssl) {
            for (sent = 0; sent < n;) {
                ERR_clear_error();
                int w = SSL_write(c.ssl, (uint8_t *)request+sent, n-sent);
                if (w > 0) { sent += w; continue; }
                int err = SSL_get_error(c.ssl, w);
                if (err == SSL_ERROR_WANT_READ && !wait_fd(c.fd, POLLIN, c.deadline)) continue;
                if (err == SSL_ERROR_WANT_WRITE && !wait_fd(c.fd, POLLOUT, c.deadline)) continue;
                break;
            }
        } else
#endif
        sent = socket_write(&c, (uint8_t *)request, n);
        if (sent != n) { rc = error(r, 55, "请求发送失败"); break; }
        if (headers(&c, &h)) { rc = error(r, 18, "响应头无效、过大或截断"); break; }
        r->status = h.status;
        int redir = h.status == 301 || h.status == 302 || h.status == 303 || h.status == 307 || h.status == 308;
        if (redir && o->follow && h.location[0]) {
            struct url next;
            if (hop == REDIRECT_MAXIMUM) { rc = error(r, 47, "重定向超过 10 跳"); break; }
            if (redirect_url(current, sizeof current, &u, h.location) || parse_url(current, &next)) { rc = error(r, 3, "无效的重定向 URL"); break; }
            if (u.tls && !next.tls) { rc = error(r, 60, "拒绝 HTTPS 降级到 HTTP"); break; }
            disconnect(&c); continue;
        }
        if (o->fail_http && h.status >= 400) { rc = error(r, 22, "HTTP 错误状态"); break; }
        if (!o->output || !strcmp(o->output, "-")) out = stdout;
        else {
            size_t len = strlen(o->output)+24;
            temp = malloc(len);
            if (!temp) { rc = error(r, 23, "输出路径分配失败"); break; }
            snprintf(temp, len, "%s.part.XXXXXX", o->output);
            int fd = mkstemp(temp);
            if (fd < 0 || !(out = fdopen(fd, "wb"))) {
                if (fd >= 0) close(fd);
                rc = error(r, 23, "无法创建临时输出文件"); break;
            }
        }
        rc = body(&c, &h, out, r, pg);
        if (fflush(out) && !rc) rc = error(r, 23, "输出刷新失败");
        if (out != stdout && fclose(out) && !rc) rc = error(r, 23, "输出关闭失败");
        out = NULL;
        if (!rc && temp && rename(temp, o->output)) rc = error(r, 23, "输出重命名失败");
        break;
    }
done:
    if (rc && (errno == ETIMEDOUT || now_ms() >= c.deadline)) rc = error(r, 28, "下载总期限已到");
    disconnect(&c);
    if (temp) { if (rc) unlink(temp); free(temp); }
    return rc;
}

/* 两个入口只保留各自命令行语义；不接受并忽略未知开关。 */
int http_cli(int argc, char **argv, int wget_mode) {
    struct http_options o = {.follow = wget_mode, .fail_http = wget_mode, .timeout = 30};
    struct http_result r;
    struct progress_ctx pctx = { .wget = wget_mode };
    struct http_progress pg = { .cb = NULL, .ud = &pctx };
    const char *name = wget_mode ? "wget" : "curl", *fmt = NULL;
    int silent = 0, show_error = 0, positional = 0;
    char filename[256];
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!positional && !strcmp(a, "--")) { positional = 1; continue; }
        if (!positional && a[0] == '-') {
            /* GNU 风格 --opt=value(wget 惯用):拆成名字+值后与空格形式同一分支。 */
            char split[128];
            const char *eq = strchr(a, '=');
            if (a[1] == '-' && eq && ((size_t)(eq - a) >= sizeof split - 1 || !eq[1])) goto usage;
            const char *val = NULL;
            if (a[1] == '-' && eq && (size_t)(eq - a) < sizeof split) {
                size_t nl = (size_t)(eq - a);
                memcpy(split, a, nl); split[nl] = 0;
                val = eq + 1;
                a = split;
            }
            if (!strcmp(a, "--help")) {
                printf("%s (Parlz/OpenSSL)\n用法: %s [%s 文件] [--insecure] [--cacert 文件] [%s 秒] URL\n默认严格校验证书(系统信任库或 --cacert)；--insecure 仅限测试。\n不支持代理/压缩/认证；总期限默认 30 秒；最多 10 次重定向。\n", name, name, wget_mode ? "-O" : "-o", wget_mode ? "-T" : "-m");
                return 0;
            } else if (!strcmp(a, "--insecure") || (!wget_mode && !strcmp(a, "-k")) || (wget_mode && !strcmp(a, "--no-check-certificate"))) o.insecure = 1;
            else if (!strcmp(a, "-L") || !strcmp(a, "--location")) o.follow = 1;
            else if (!strcmp(a, "--no-redirect")) o.follow = 0;
            else if (!strcmp(a, "-s") || !strcmp(a, "--silent") || (wget_mode && (!strcmp(a, "-q") || !strcmp(a, "--quiet")))) silent = 1;
            else if (!strcmp(a, "-S") || !strcmp(a, "--show-error")) show_error = 1;
            else if (!strcmp(a, "-sS") || !strcmp(a, "-Ss")) { silent = 1; show_error = 1; }
            else if (!strcmp(a, "-f") || !strcmp(a, "--fail")) o.fail_http = 1;
            else if (!strcmp(a, "--no-progress") || (wget_mode && !strcmp(a, "-N"))) pctx.quiet = 1;
            else if (!wget_mode && !strcmp(a, "-#")) pctx.quiet = 0;
            else if (!strcmp(a, "--cacert") || !strcmp(a, "--ca-certificate") || !strcmp(a, "--ca-directory")) {
                if (val) o.cacert = val;
                else {
                    if (++i == argc) goto usage;
                    o.cacert = argv[i];
                }
            } else if (!strcmp(a, wget_mode ? "-O" : "-o") || !strcmp(a, wget_mode ? "--output-document" : "--output")) {
                if (val) o.output = val;
                else {
                    if (++i == argc) goto usage;
                    o.output = argv[i];
                }
            } else if (!wget_mode && (!strcmp(a, "-w") || !strcmp(a, "--write-out"))) {
                if (val) fmt = val;
                else {
                    if (++i == argc) goto usage;
                    fmt = argv[i];
                }
            } else if (!strcmp(a, wget_mode ? "-T" : "-m") || !strcmp(a, wget_mode ? "--timeout" : "--max-time")) {
                const char *end; uint64_t t;
                const char *src = val;
                if (!src) {
                    if (++i == argc) goto usage;
                    src = argv[i];
                }
                if (number(src, 10, &t, &end) || *end || t < 1 || t > 86400) goto usage;
                o.timeout = (int)t;
            } else goto usage;
        } else { if (o.url) goto usage; o.url = a; }
    }
    if (!o.url) goto usage;
    /* 进度显示:wget/curl 默认都开(写 stderr,不影响正文);
     * 非 TTY(管道/重定向)也照打;--silent 或 --no-progress/-N 关掉。 */
    pg.cb = progress_cb;
    if (silent)
        pctx.quiet = 1;
    if (wget_mode && !o.output) {
        char expanded[URL_MAX]; struct url u;
        int n = snprintf(expanded, sizeof expanded, "%s%s", strstr(o.url, "://") ? "" : "http://", o.url);
        if (n < 0 || (size_t)n >= sizeof expanded || parse_url(expanded, &u)) goto usage;
        u.path[strcspn(u.path, "?")] = 0;
        const char *p = strrchr(u.path, '/'); p = p ? p+1 : u.path;
        if (!*p || !strcmp(p, ".") || !strcmp(p, "..")) p = "index.html";
        if (copy_text(filename, sizeof filename, p, strlen(p))) goto usage;
        o.output = filename;
    }
    int rc = http_download(&o, &r, &pg);
    if (rc && (!silent || show_error)) fprintf(stderr, "%s: %s (HTTP %03d)\n", name, r.error, r.status);
    if (!rc && wget_mode && !silent) fprintf(stderr, "wget: 已保存 %llu 字节到 %s\n", (unsigned long long)r.bytes, o.output);
    if (fmt) {
        for (const char *p = fmt; *p;) {
            if (!strncmp(p, "%{http_code}", 12)) { printf("%03d", r.status); p += 12; }
            else if (p[0] == '\\' && p[1] == 'n') { putchar('\n'); p += 2; }
            else putchar(*p++);
        }
        if (fflush(stdout) && !rc) rc = 23;
    }
    return rc;
usage:
    fprintf(stderr, "%s: 无效或缺少参数；使用 --help\n", name);
    return 2;
}
