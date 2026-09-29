/* handlers.c - static, proxy, cgi, redirect + helpers */
#include "pweb.h"
#ifndef PWEB_PLATFORM_WIN
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif
#include <dirent.h>

static int send_all(int fd, const char *b, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = PWEB_SEND(fd, b + off, n - off, 0);
        if (w <= 0) { return -1; }
        off += (size_t)w;
    }
    return 0;
}

static char *build_head(char *hdrs, int status, int keepalive,
                        const char *ctype, size_t body_len)
{
    char reason[48];
    reason_str(status, reason, sizeof(reason));
    char *h = hdrs;
    h += sprintf(h, "HTTP/1.1 %d %s\r\n", status, reason);
    h += sprintf(h, "Content-Type: %s\r\n", ctype ? ctype : "text/plain");
    h += sprintf(h, "Server: PWeb\r\n");
    h += sprintf(h, "Content-Length: %zu\r\n", body_len);
    h += sprintf(h, "Connection: %s\r\n", keepalive ? "keep-alive" : "close");
    h += sprintf(h, "\r\n");
    return h;
}

int resp_send(int fd, int status, int keepalive, const char *body,
              const char *ctype, int extra_hdrs, char *hdrs)
{
    size_t blen = body ? strlen(body) : 0;
    char hbuf[512];
    char *h;
    if (hdrs) {
        h = hdrs;
    } else {
        h = build_head(hbuf, status, keepalive, ctype, blen);
        if (extra_hdrs == 0) {
            if (send_all(fd, hbuf, (size_t)(h - hbuf)) < 0) { return -1; }
            if (body && send_all(fd, body, blen) < 0) { return -1; }
            return 0;
        }
    }
    const char *base = hdrs ? hdrs : hbuf;
    if (send_all(fd, base, (size_t)(h - base)) < 0) { return -1; }
    if (body && send_all(fd, body, blen) < 0) { return -1; }
    return 0;
}

/* ================= static ================= */
int handle_static(int fd, const vhost_t *vh, const loc_t *lc, const req_t *req,
                  const char *client_ip, const char *uri)
{
    (void)client_ip;
    char path[1024];
    const char *root = lc->root[0] ? lc->root : vh->root;
    if (root[0] == 0) {
        char body[32];
        snprintf(body, sizeof(body), "500 no root");
        char h[512];
        char *hp = build_head(h, 500, 0, "text/plain", strlen(body));
        send_all(fd, h, (size_t)(hp - h));
        send_all(fd, body, strlen(body));
        return 0;
    }
    snprintf(path, sizeof(path), "%s%s", root, uri);

    struct stat st;
    int is_binary = 0;
    int keepalive = 1;
    const char *conn = req_hdr(req, "Connection");
    if (conn && casestrs(conn, "close")) { keepalive = 0; }

    if (stat(path, &st) != 0) {
        char p2[1100];
        snprintf(p2, sizeof(p2), "%s/", path);
        if (stat(p2, &st) == 0 && S_ISDIR(st.st_mode)) {
            char loc[1200];
            snprintf(loc, sizeof(loc), "%s/", uri);
            char h[512];
            char *hp = h;
            hp += sprintf(hp, "HTTP/1.1 301 Moved Permanently\r\n");
            hp += sprintf(hp, "Location: %s\r\n", loc);
            hp += sprintf(hp, "Server: PWeb\r\n");
            hp += sprintf(hp, "Content-Length: 0\r\n");
            hp += sprintf(hp, "Connection: keep-alive\r\n\r\n");
            send_all(fd, h, (size_t)(hp - h));
            return 0;
        }
        char body[32];
        snprintf(body, sizeof(body), "404 Not Found");
        char h[512];
        char *hp = build_head(h, 404, 0, "text/plain", strlen(body));
        send_all(fd, h, (size_t)(hp - h));
        send_all(fd, body, strlen(body));
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        char idxpath[1400];
        const char *il = vh->index_list[0] ? vh->index_list : "index.html";
        char il_copy[128];
        snprintf(il_copy, sizeof(il_copy), "%s", il);
        char *save = NULL;
        char *tok = strtok_r(il_copy, " ", &save);
        int found = 0;
        while (tok) {
            snprintf(idxpath, sizeof(idxpath), "%s%s/%s", root, uri, tok);
            struct stat st2;
            if (stat(idxpath, &st2) == 0 && S_ISREG(st2.st_mode)) {
                snprintf(path, sizeof(path), "%s", idxpath);
                found = 1;
                break;
            }
            tok = strtok_r(NULL, " ", &save);
        }
        if (!found) {
            if (vh->autoindex) {
                str html; str_init_m(&html);
                autoindex_page(&html, root, uri, vh->index_list);
                char h[512];
                char *hp = build_head(h, 200, keepalive,
                                      "text/html; charset=utf-8", html.len);
                send_all(fd, h, (size_t)(hp - h));
                send_all(fd, html.buf, html.len);
                str_free(&html);
                return 0;
            }
            char body[32];
            snprintf(body, sizeof(body), "403 Forbidden");
            char h[512];
            char *hp = build_head(h, 403, 0, "text/plain", strlen(body));
            send_all(fd, h, (size_t)(hp - h));
            send_all(fd, body, strlen(body));
            return 0;
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        char body[32];
        snprintf(body, sizeof(body), "404 Not Found");
        char h[512];
        char *hp = build_head(h, 404, 0, "text/plain", strlen(body));
        send_all(fd, h, (size_t)(hp - h));
        send_all(fd, body, strlen(body));
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (vh->auth_file[0]) {
        const char *auth = req_hdr(req, "Authorization");
        if (!auth || strncasecmp(auth, "Basic", 5) != 0 ||
            !verify_basic_auth(vh->auth_file, auth + 5)) {
            char h[512];
            char *hp = h;
            hp += sprintf(hp, "HTTP/1.1 401 Unauthorized\r\n");
            hp += sprintf(hp,
                          "WWW-Authenticate: Basic realm=\"%s\"\r\n",
                          vh->auth_realm[0] ? vh->auth_realm : "PWeb");
            hp += sprintf(hp, "Server: PWeb\r\n");
            hp += sprintf(hp, "Content-Length: 0\r\n");
            hp += sprintf(hp, "Connection: close\r\n\r\n");
            send_all(fd, h, (size_t)(hp - h));
            fclose(f);
            return 0;
        }
    }

    char *body = (char*)malloc(size + 1);
    if (!body) { fclose(f); return 0; }
    size_t rd = fread(body, 1, (size_t)size, f);
    fclose(f);
    body[rd] = 0;

    const char *mime = mime_for(path, &is_binary);
    int use_gzip = 0;
    if (vh->gzip && !is_binary && (long)rd >= (long)vh->gzip_min) {
        const char *ae = req_hdr(req, "Accept-Encoding");
        if (ae && casestrs(ae, "gzip")) {
            use_gzip = 1;
        }
    }
    if (use_gzip) {
        size_t glen = 0;
        char *gz = pweb_gzip(body, rd, &glen);
        if (gz) {
            char h[512];
            char *hp = h;
            hp += sprintf(hp, "HTTP/1.1 200 OK\r\n");
            hp += sprintf(hp, "Content-Type: %s\r\n", mime);
            hp += sprintf(hp, "Content-Encoding: gzip\r\n");
            hp += sprintf(hp, "Vary: Accept-Encoding\r\n");
            hp += sprintf(hp, "Server: PWeb\r\n");
            hp += sprintf(hp, "Content-Length: %zu\r\n", glen);
            hp += sprintf(hp, "Connection: %s\r\n",
                          keepalive ? "keep-alive" : "close");
            hp += sprintf(hp, "\r\n");
            send_all(fd, h, (size_t)(hp - h));
            send_all(fd, gz, glen);
            free(gz);
            free(body);
            return 0;
        }
    }
    char h[512];
    char *hp = build_head(h, 200, keepalive, mime, rd);
    send_all(fd, h, (size_t)(hp - h));
    send_all(fd, body, rd);
    free(body);
    return 0;
}

/* htpasswd verify: line "user:plain" or "user:$md5$hex32" */
int verify_basic_auth(const char *file, const char *b64auth)
{
    while (*b64auth == ' ') { b64auth++; }
    size_t cl = 0;
    char *creds = b64_decode(b64auth, strlen(b64auth), &cl);
    if (!creds) { return 0; }
    char user[256], pass[256];
    char *cc = strchr(creds, ':');
    if (!cc) { free(creds); return 0; }
    size_t ul = (size_t)(cc - creds);
    if (ul >= sizeof(user)) { ul = sizeof(user) - 1; }
    memcpy(user, creds, ul); user[ul] = 0;
    snprintf(pass, sizeof(pass), "%s", cc + 1);
    FILE *af = fopen(file, "r");
    int ok = 0;
    if (af) {
        char line[512];
        while (fgets(line, sizeof(line), af)) {
            char *lcc = strchr(line, ':');
            if (!lcc) { continue; }
            *lcc = 0;
            char *lu = line;
            char *lp = lcc + 1;
            char *ll = lp;
            while (*ll && *ll != '\n' && *ll != '\r') { ll++; }
            *ll = 0;
            if (strcmp(lu, user) == 0) {
                if (strncmp(lp, "$md5$", 5) == 0) {
                    char expected[33];
                    md5_hex(pass, strlen(pass), expected);
                    if (strncmp(lp + 5, expected, 32) == 0) {
                        ok = 1; break;
                    }
                } else if (strcmp(lp, pass) == 0) {
                    ok = 1; break;
                }
            }
        }
        fclose(af);
    }
    free(creds);
    return ok;
}

#ifndef PWEB_PLATFORM_WIN
/* ================= autoindex ================= */
int autoindex_page(str *s, const char *root, const char *uri,
                   const char *parent)
{
    str_appendf(s, "<html><body>autoindex disabled on Windows</body></html>");
    (void)root; (void)uri; (void)parent;
    return 0;
}
#else
int autoindex_page(str *s, const char *root, const char *uri,
                   const char *parent)
{
    char dirpath[1200];
    snprintf(dirpath, sizeof(dirpath), "%s%s", root, uri);
    DIR *d = opendir(dirpath);
    if (!d) {
        str_appendf(s, "<html><body>404</body></html>");
        return -1;
    }
    char parent_esc[512] = "";
    if (parent && strlen(parent) > 1) {
        str esc; str_init_m(&esc);
        url_encode(&esc, parent, 1);
        snprintf(parent_esc, sizeof(parent_esc),
                 "<a href=\"%s/\">../</a>\n", esc.buf);
        str_free(&esc);
    }
    str_appendf(s,
        "<!doctype html><html><head><meta charset=utf-8>"
        "<title>Index of %s</title></head>"
        "<body><h1>Index of %s</h1><ul>", uri, uri);
    if (parent_esc[0]) {
        str_appendf(s, "<li>%s", parent_esc);
    }
    char names[512][256];
    char sizesc[512][16];
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0) { continue; }
        if (n < 512) {
            snprintf(names[n], sizeof(names[n]), "%s", ent->d_name);
            char p2[1400];
            snprintf(p2, sizeof(p2), "%s%s%s", root, uri, ent->d_name);
            struct stat st;
            if (stat(p2, &st) == 0) {
                snprintf(sizesc[n], sizeof(sizesc[n]), "%ld",
                         (long)st.st_size);
            }
            n++;
        }
    }
    closedir(d);
    for (int i = 1; i < n; i++) {
        char tn[256], ts[16];
        strcpy(tn, names[i]); strcpy(ts, sizesc[i]);
        int j = i - 1;
        while (j >= 0 && strcasecmp(names[j], tn) > 0) {
            strcpy(names[j+1], names[j]);
            strcpy(sizesc[j+1], sizesc[j]);
            j--;
        }
        strcpy(names[j+1], tn);
        strcpy(sizesc[j+1], ts);
    }
    for (int i = 0; i < n; i++) {
        str esc; str_init_m(&esc);
        url_encode(&esc, names[i], 1);
        char p2[1400];
        snprintf(p2, sizeof(p2), "%s%s%s", root, uri, names[i]);
        struct stat st;
        int isdir = (stat(p2, &st) == 0 && S_ISDIR(st.st_mode));
        char link[512];
        if (isdir) {
            snprintf(link, sizeof(link), "%s/", esc.buf);
        } else {
            snprintf(link, sizeof(link), "%s", esc.buf);
        }
        str_appendf(s, "<li><a href=\"%s\">%s</a> %s<br>\n",
                    link, names[i], sizesc[i]);
        str_free(&esc);
    }
    str_append(s, "</ul></body></html>");
    return 0;
}


#endif /* PWEB_PLATFORM_WIN autoindex */

/* ================= proxy ================= */
#ifndef PWEB_PLATFORM_WIN
/* send a fixed upstream error response to the client and finish. */
static void proxy_fail(int fd, int status, const char *msg)
{
    char hbuf[512];
    char *hp = build_head(hbuf, status, 0, "text/plain", strlen(msg));
    send_all(fd, hbuf, (size_t)(hp - hbuf));
    send_all(fd, msg, strlen(msg));
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
#if PWEB_SSL
        is_https = 1;
        h += 8;
        port = 443;
#else
        proxy_fail(fd, 501, "501 https not supported (build with SSL=1)");
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

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        proxy_fail(fd, 502, "502 Bad Gateway: cannot resolve upstream");
        return 0;
    }
    int up = PWEB_SOCKET(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (up < 0 || PWEB_CONNECT(up, res->ai_addr, res->ai_addrlen) < 0) {
        freeaddrinfo(res);
        if (up >= 0) { close(up); }
        proxy_fail(fd, 502, "502 Bad Gateway: connect failed");
        return 0;
    }
    freeaddrinfo(res);

#if PWEB_SSL
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    if (is_https) {
        ssl_ctx = PWEB_SSL_CTX_NEW();
        if (!ssl_ctx) { close(up); proxy_fail(fd, 502, "502 TLS init failed"); return 0; }
        ssl = PWEB_SSL_NEW(ssl_ctx);
        if (!ssl) { PWEB_SSL_CTX_FREE(ssl_ctx); close(up);
                    proxy_fail(fd, 502, "502 TLS init failed"); return 0; }
        PWEB_SSL_SET_FD(ssl, up);
        if (PWEB_SSL_CONNECT(ssl) != 1) {
            PWEB_SSL_FREE(ssl); PWEB_SSL_CTX_FREE(ssl_ctx); close(up);
            proxy_fail(fd, 502, "502 TLS handshake failed");
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
#if PWEB_SSL
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

    /* send the upstream request */
#if PWEB_SSL
    if (is_https) {
        if (PWEB_SSL_WRITE(ssl, fwd.buf, (int)fwd.len) <= 0) {
            PWEB_SSL_SHUTDOWN(ssl); PWEB_SSL_FREE(ssl); PWEB_SSL_CTX_FREE(ssl_ctx);
            close(up); str_free(&fwd);
            proxy_fail(fd, 502, "502 TLS send failed");
            return 0;
        }
    } else
#endif
    {
        if (send_all(up, fwd.buf, fwd.len) < 0) {
            close(up); str_free(&fwd);
            proxy_fail(fd, 502, "502 Bad Gateway: send failed");
            return 0;
        }
    }
    str_free(&fwd);

    /* stream the upstream response back to the client (8 KB chunks). */
    char buf[8192];
    for (;;) {
        int r;
#if PWEB_SSL
        if (is_https) {
            r = PWEB_SSL_READ(ssl, buf, sizeof(buf));
        } else
#endif
        {
            r = (int)PWEB_RECV(up, buf, sizeof(buf), 0);
        }
        if (r <= 0) { break; }
        if (send_all(fd, buf, (size_t)r) < 0) { break; }
    }

#if PWEB_SSL
    if (is_https) { PWEB_SSL_SHUTDOWN(ssl); PWEB_SSL_FREE(ssl); PWEB_SSL_CTX_FREE(ssl_ctx); }
#endif
    close(up);
    return 0;
}

#endif /* PWEB_PLATFORM_WIN proxy */

/* ================= cgi ================= */
#ifndef PWEB_PLATFORM_WIN
int handle_cgi(int fd, const vhost_t *vh, const loc_t *lc, const req_t *req,
               const char *client_ip)
{
    /* Resolve `root` relative to PWeb's CWD so the CGI's disk path is
     * correct regardless of whether the config uses a relative or an
     * absolute root (PWeb is launched from the config dir). */
    char root_resolved[1024];
    {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) == 0) {
            snprintf(cwd, sizeof(cwd), ".");
        }
        const char *root = lc->root[0] ? lc->root : vh->root;
        if (root[0] == '/') {
            snprintf(root_resolved, sizeof(root_resolved), "%s", root);
        } else {
            snprintf(root_resolved, sizeof(root_resolved), "%s/%s", cwd, root);
        }
    }
    char pathonly_buf[1024];
    char query[1024];
    char disk[1200];
    snprintf(pathonly_buf, sizeof(pathonly_buf), "%s", req->target);
    char *qm = strchr(pathonly_buf, '?');
    if (qm) {
        *qm = 0;
        snprintf(query, sizeof(query), "%s", qm + 1);
    } else {
        query[0] = 0;
    }
    snprintf(disk, sizeof(disk), "%s%s", root_resolved, pathonly_buf);
    struct stat st;
    if (stat(disk, &st) != 0) {
        char body[64];
        snprintf(body, sizeof(body),
                 "500 Internal Server Error: script not found");
        char hbuf[512];
        char *hp = build_head(hbuf, 500, 0, "text/plain", strlen(body));
        send_all(fd, hbuf, (size_t)(hp - hbuf));
        send_all(fd, body, strlen(body));
        return 0;
    }
    /* cgi_pass may be "script" or "interp script" (nginx-style). Split on the
     * first space; the interpreter is exec'd with the remaining script path
     * and the on-disk target as argv. */
    char script[600];
    snprintf(script, sizeof(script), "%s", lc->cgi_script);
    char interp[256] = "";
    char script_path[512];
    char *sp = strchr(script, ' ');
    if (sp) {
        size_t il = (size_t)(sp - script);
        if (il >= sizeof(interp)) { il = sizeof(interp) - 1; }
        memcpy(interp, script, il);
        interp[il] = 0;
        snprintf(script_path, sizeof(script_path), "%s", sp + 1);
        while (*sp == ' ') { sp++; }
    } else {
        snprintf(script_path, sizeof(script_path), "%s", script);
    }

    int pfd[2];   /* CGI stdout -> PWeb */
    int cfd[2];   /* PWeb request body -> CGI stdin */
    if (pipe(pfd) != 0 || pipe(cfd) != 0) {
        char body[32];
        snprintf(body, sizeof(body), "500 pipe failed");
        char hbuf[512];
        char *hp = build_head(hbuf, 500, 0, "text/plain", strlen(body));
        send_all(fd, hbuf, (size_t)(hp - hbuf));
        send_all(fd, body, strlen(body));
        return 0;
    }
    int pid = fork();
    if (pid < 0) {
        char body[32];
        snprintf(body, sizeof(body), "500 fork failed");
        char hbuf[512];
        char *hp = build_head(hbuf, 500, 0, "text/plain", strlen(body));
        send_all(fd, hbuf, (size_t)(hp - hbuf));
        send_all(fd, body, strlen(body));
        return 0;
    }
    if (pid == 0) {
        /* child: stdout -> response pipe; stdin -> body pipe.
         * dup a copy of the parent's stderr so the CGI's own stderr still
         * reaches the operator log while its stdout is the response pipe. */
        int cerr = dup(2);
        int r1 = dup2(pfd[1], 1);
        int r0 = dup2(cfd[0], 0);
        if (r1 != 0 || r0 != 0) { _exit(127); }
        /* Child only needs the open ends; close the rest so the parent
         * sees EOF when the CGI exits. */
        close(pfd[0]);
        close(cfd[1]);
        char clen[32];
        int body_len = req->content_length >= 0 ? req->content_length : 0;
        snprintf(clen, sizeof(clen), "%d", body_len);
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        setenv("REQUEST_METHOD", req->method, 1);
        setenv("SCRIPT_NAME", pathonly_buf, 1);
        setenv("QUERY_STRING", query, 1);
        setenv("REMOTE_ADDR", client_ip, 1);
        setenv("REMOTE_PORT", "80", 1);
        setenv("SERVER_SOFTWARE", "PWeb", 1);
        setenv("SERVER_NAME", "pweb", 1);
        setenv("PWEB_ROOT", root_resolved, 1); /* shim uses this to locate the script */
        const char *ha = req_hdr(req, "Host");
        const char *ua = req_hdr(req, "User-Agent");
        setenv("HTTP_HOST", ha ? ha : "", 1);
        setenv("HTTP_USER_AGENT", ua ? ua : "", 1);
        setenv("CONTENT_LENGTH", clen, 1);
        char *argvx[8];
        argvx[0] = script_path;
        argvx[1] = disk;
        argvx[2] = NULL;
        if (interp[0]) {
            /* cgi_pass "interp script": exec the interpreter with the script
             * path as its first arg (nginx-style, e.g. `/bin/sh shim.sh`). */
            char *argvi[4];
            argvi[0] = interp;
            argvi[1] = script_path;
            argvi[2] = disk;
            argvi[3] = NULL;
            execv(interp, argvi);
            execv("/bin/sh", argvi); /* fall back if interp missing */
        } else {
            execv(script_path, argvx);
        }
        /* If execv lands here, it failed. cerr still points at the
         * operator log (fd 2 was NOT redirected, only fd 1/0 were). */
        int ee = errno;
        char dbg[512];
        snprintf(dbg, sizeof(dbg),
                 "pweb: CGI exec failed: interp=%s script=%s disk=%s "
                 "root=%s errno=%d (%s)\n",
                 interp[0] ? interp : "(direct)", script_path, disk,
                 root_resolved, ee, strerror(ee));
        ssize_t dw = write(cerr, dbg, strlen(dbg));
        (void)dw;
        close(cerr);
        _exit(127);
    }
    /* parent: close child ends we don't use */
    close(pfd[1]);
    close(cfd[0]);
    /* write the request body into the CGI's stdin, then close so it sees EOF */
    if (req->body && req->content_length > 0) {
        ssize_t off = 0;
        int blen = req->content_length;
        while (off < blen) {
            ssize_t w = write(cfd[1], req->body + off, (size_t)(blen - off));
            if (w <= 0) { break; }
            off += w;
        }
    }
    close(cfd[1]);
    char buf[8192];
    size_t total_out = 0;
    for (;;) {
        ssize_t r = read(pfd[0], buf, sizeof(buf));
        if (r <= 0) { break; }
        if (send_all(fd, buf, (size_t)r) < 0) { break; }
        total_out += (size_t)r;
    }
    close(pfd[0]);
    if (total_out == 0) {
        /* CGI produced nothing (or exec failed): send a minimal 500 so the
         * client gets a complete response and keep-alive works. */
        char hb[128];
        char *hp = hb;
        hp += sprintf(hp, "HTTP/1.1 500 Internal Server Error\r\n");
        hp += sprintf(hp, "Content-Type: text/plain\r\n");
        hp += sprintf(hp, "Content-Length: 16\r\n");
        hp += sprintf(hp, "Connection: close\r\n\r\n");
        hp += sprintf(hp, "CGI no output");
        send_all(fd, hb, (size_t)(hp - hb));
    }
    int st2;
    waitpid(pid, &st2, 0);
    /* Log the CGI outcome. Two cases:
     *  - total_out > 0 and exit 0: normal, log at debug level only.
     *  - anything else: write the full diagnostic (exit code, signal,
     *    errno) to stderr so misconfigured cgi_pass is visible. */
    if (total_out == 0 || !WIFEXITED(st2) || WEXITSTATUS(st2) != 0) {
        char dbg[448];
        int code = WIFEXITED(st2) ? WEXITSTATUS(st2) : (-1 - (WIFSIGNALED(st2) ? WTERMSIG(st2) : 0));
        char extra[160];
        extra[0] = 0;
        if (interp[0] && access(interp, X_OK) != 0) {
            snprintf(extra, sizeof(extra), " [interp %s not X_OK]", interp);
        } else if (script_path[0] && access(script_path, X_OK) != 0) {
            snprintf(extra, sizeof(extra), " [script not X_OK]");
        }
        snprintf(dbg, sizeof(dbg),
                 "pweb: cgi interp=%s script=%s exit=%d out=%zu%s\n",
                 interp[0] ? interp : "(direct)", script_path,
                 code, total_out, extra);
        write(2, dbg, strlen(dbg));
    }
    return 0;
}


#endif /* PWEB_PLATFORM_WIN cgi */
/* ================= redirect ================= */
int handle_redir(int fd, const loc_t *lc, const req_t *req, const char *uri)
{
    char target[600] = "";
    /* If the location carries a rewrite pattern, expand $1..$9 using the
     * request URI. Otherwise fall back to the legacy fixed redir string. */
    if (lc->redir_pat[0]) {
        int ok = pweb_re_sub(lc->redir_pat, uri, lc->redir_repl,
                             target, sizeof(target));
        if (!ok) {
            /* pattern didn't match: fall back to raw replacement */
            snprintf(target, sizeof(target), "%s", lc->redir_repl);
        }
    } else {
        /* legacy "regex repl" combined field */
        const char *redir = lc->redir;
        const char *sp = strchr(redir, ' ');
        if (sp) {
            size_t l = strlen(sp + 1);
            if (l >= sizeof(target)) { l = sizeof(target) - 1; }
            memcpy(target, sp + 1, l);
            target[l] = 0;
        }
    }
    const char *status = lc->redir_flag ? "302 Found" : "301 Moved Permanently";
    char h[512];
    char *hp = h;
    hp += sprintf(hp, "HTTP/1.1 %s\r\n", status);
    hp += sprintf(hp, "Location: %s\r\n", target);
    hp += sprintf(hp, "Server: PWeb\r\n");
    hp += sprintf(hp, "Content-Length: 0\r\n");
    hp += sprintf(hp, "Connection: keep-alive\r\n\r\n");
    send_all(fd, h, (size_t)(hp - h));
    (void)req;
    return 0;
}
