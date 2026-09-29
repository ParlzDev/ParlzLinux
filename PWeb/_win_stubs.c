/* _win_stubs.c - Windows implementations of proxy / cgi / autoindex */
#include "pweb.h"

#if PWEB_PLATFORM_WIN

static void send_simple(int fd, int code, const char *reason,
                        const char *body)
{
    char hb[512];
    char *hp = hb;
    hp += sprintf(hp, "HTTP/1.1 %d %s\r\n", code, reason);
    hp += sprintf(hp, "Content-Type: text/plain\r\n");
    hp += sprintf(hp, "Content-Length: %d\r\n", (int)strlen(body));
    hp += sprintf(hp, "Connection: close\r\n\r\n");
    PWEB_SEND(fd, hb, (size_t)(hp - hb), 0);
    PWEB_SEND(fd, body, strlen(body), 0);
}

int handle_proxy(int fd, const loc_t *lc, const req_t *req,
                 const char *client_ip)
{
    const char *pp = lc->proxypass;
    char host[256];
    int port = 80;
    int is_https = 0;
    const char *h = pp;
    if (strncasecmp(h, "http://", 7) == 0) { h += 7; }
    else if (strncasecmp(h, "https://", 8) == 0) {
#ifdef PWEB_SSL
        is_https = 1;
        h += 8;
        port = 443;
#else
        send_simple(fd, 501, "Not Implemented",
                    "501 https not supported (build with SSL=1)");
        return 0;
#endif
    }
    char *p = (char*)h;
    char *cp = strchr(p, ':');
    if (cp) {
        size_t hl = (size_t)(cp - p);
        if (hl >= sizeof(host)) { hl = sizeof(host) - 1; }
        memcpy(host, p, hl); host[hl] = 0;
        port = atoi(cp + 1);
        p = cp + 1;
    } else {
        snprintf(host, sizeof(host), "%s", p);
    }
    char fwd_path[512] = "";
    while (*p == '/') { p++; }
    {
        char *sl = p;
        while (*sl) { sl++; }
        if (sl != p) {
            size_t pl = (size_t)(sl - p);
            if (pl >= sizeof(fwd_path)) { pl = sizeof(fwd_path) - 1; }
            memcpy(fwd_path, p, pl); fwd_path[pl] = 0;
        }
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    unsigned long ip = inet_addr(host);
    if (ip == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            send_simple(fd, 502, "Bad Gateway",
                        "502 cannot resolve upstream");
            return 0;
        }
        sa.sin_addr.s_addr = *(unsigned long*)he->h_addr_list[0];
    } else {
        sa.sin_addr.s_addr = ip;
    }

    int up = PWEB_SOCKET(AF_INET, SOCK_STREAM, 0);
    if (up < 0 || PWEB_CONNECT(up, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        if (up >= 0) { PWEB_CLOSE(up); }
        send_simple(fd, 502, "Bad Gateway", "502 connect failed");
        return 0;
    }

#ifdef PWEB_SSL
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    if (is_https) {
        ssl_ctx = PWEB_SSL_CTX_NEW();
        if (!ssl_ctx) { PWEB_CLOSE(up);
            send_simple(fd, 502, "Bad Gateway", "502 TLS init failed");
            return 0; }
        ssl = PWEB_SSL_NEW(ssl_ctx);
        if (!ssl) { PWEB_SSL_CTX_FREE(ssl_ctx); PWEB_CLOSE(up);
            send_simple(fd, 502, "Bad Gateway", "502 TLS init failed");
            return 0; }
        PWEB_SSL_SET_FD(ssl, up);
        if (PWEB_SSL_CONNECT(ssl) != 1) {
            PWEB_SSL_FREE(ssl); PWEB_SSL_CTX_FREE(ssl_ctx); PWEB_CLOSE(up);
            send_simple(fd, 502, "Bad Gateway", "502 TLS handshake failed");
            return 0;
        }
    }
#endif

    char target[2048];
    if (fwd_path[0]) {
        snprintf(target, sizeof(target), "%s%s", fwd_path, req->target);
    } else {
        snprintf(target, sizeof(target), "%s", req->target);
    }
    str fwd; str_init_m(&fwd);
    str_appendf(&fwd, "%s %s HTTP/1.1\r\n", req->method, target);
    str_appendf(&fwd, "Host: %s:%d\r\n", host, port);
    str_appendf(&fwd, "X-Forwarded-For: %s\r\n", client_ip);
#ifdef PWEB_SSL
    str_append(&fwd, is_https ? "X-Forwarded-Proto: https\r\n"
                              : "X-Forwarded-Proto: http\r\n");
#else
    str_append(&fwd, "X-Forwarded-Proto: http\r\n");
#endif
    for (int i = 0; i < req->n_hdrs; i++) {
        if (strcasecmp(req->hdrs[i].k, "Host") == 0) { continue; }
        if (strcasecmp(req->hdrs[i].k, "Connection") == 0) { continue; }
        if (strcasecmp(req->hdrs[i].k, "Proxy-Connection") == 0) { continue; }
        str_appendf(&fwd, "%s: %s\r\n", req->hdrs[i].k, req->hdrs[i].v);
    }
    str_append(&fwd, "\r\n");
    int body_len = req->content_length >= 0 ? req->content_length : 0;
    if (body_len > 0 && req->body) {
        str_append_raw(&fwd, req->body, body_len);
    }

    int ok = 1;
#ifdef PWEB_SSL
    if (is_https) {
        if (PWEB_SSL_WRITE(ssl, fwd.buf, (int)fwd.len) <= 0) { ok = 0; }
    } else
#endif
    {
        size_t sent = 0;
        while (sent < fwd.len) {
            int w = PWEB_SEND(up, fwd.buf + sent, fwd.len - sent, 0);
            if (w <= 0) { ok = 0; break; }
            sent += (size_t)w;
        }
    }
    if (!ok) {
#ifdef PWEB_SSL
        if (is_https) { PWEB_SSL_SHUTDOWN(ssl); PWEB_SSL_FREE(ssl); PWEB_SSL_CTX_FREE(ssl_ctx); }
#endif
        PWEB_CLOSE(up);
        str_free(&fwd);
        send_simple(fd, 502, "Bad Gateway", "502 send failed");
        return 0;
    }
    str_free(&fwd);

    char buf[8192];
    for (;;) {
        int r;
#ifdef PWEB_SSL
        if (is_https) { r = PWEB_SSL_READ(ssl, buf, sizeof(buf)); }
        else
#endif
        { r = PWEB_RECV(up, buf, sizeof(buf), 0); }
        if (r <= 0) { break; }
        size_t s2 = 0;
        while (s2 < (size_t)r) {
            int w = PWEB_SEND(fd, buf + s2, r - s2, 0);
            if (w <= 0) { break; }
            s2 += (size_t)w;
            if (s2 >= (size_t)r) { break; }
        }
    }
#ifdef PWEB_SSL
    if (is_https) { PWEB_SSL_SHUTDOWN(ssl); PWEB_SSL_FREE(ssl); PWEB_SSL_CTX_FREE(ssl_ctx); }
#endif
    PWEB_CLOSE(up);
    return 0;
}

int handle_cgi(int fd, const vhost_t *vh, const loc_t *lc, const req_t *req,
               const char *client_ip)
{
    send_simple(fd, 501, "Not Implemented",
                "CGI requires POSIX fork/exec; unavailable on Windows");
    (void)vh; (void)lc; (void)req; (void)client_ip;
    return 0;
}

#endif /* PWEB_PLATFORM_WIN */
