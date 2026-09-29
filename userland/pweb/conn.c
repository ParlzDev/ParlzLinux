/* conn.c - connection accept + per-request dispatch */
#include "pweb.h"
#include <stdio.h>

cfg_t *g_cfg = NULL;
volatile int g_running = 1;
int g_verbose = 0;

/* pick the location that matches the request target. Returns NULL when no
 * location matched (caller falls back to the vhost default static root). */
const loc_t *locate(const vhost_t *vh, const char *target, int *is_default)
{
    *is_default = 1;
    int best = -1;
    size_t best_len = 0;
    for (int i = 0; i < vh->nlocs; i++) {
        const loc_t *lc = &vh->locs[i];
        if (lc->is_regex) {
            /* regex location: match the pattern against the target */
            char caps[9][512];
            int nc = 0;
            if (pweb_re_match(lc->match, target, caps, &nc)) {
                *is_default = 0;
                return lc;
            }
            continue;
        }
        const char *m = lc->match;
        int exact = 0;
        if (*m == '=') {
            m++;
            while (*m == ' ') { m++; }
            if (strcmp(m, target) == 0) {
                *is_default = 0;
                return lc;
            }
            exact = 1;
        }
        if (exact) { continue; }
        size_t ml = strlen(m);
        if (ml > 0 && strncmp(m, target, ml) == 0) {
            if (ml > best_len) {
                best_len = ml;
                best = i;
            }
        }
    }
    if (best >= 0) {
        *is_default = 0;
        return &vh->locs[best];
    }
    return NULL;
}

void g_handle_conn(int fd, const struct sockaddr_in *cli, int vh_idx)
{
    int g_port = 0;
    if (cli) { g_port = ntohs(cli->sin_port); }
    char ipstr[64] = "0.0.0.0";
    if (cli) {
        PWEB_INET_NTOP(AF_INET, &cli->sin_addr, ipstr, sizeof(ipstr));
    }

#if PWEB_SSL
    /* If this vhost listens on TLS, terminate the handshake on the client
     * socket before HTTP parsing. When no cert is configured we still
     * require PWEB_SSL_HAVE; otherwise the connection is closed. */
    {
        const vhost_t *vh0 = vhost_find(g_cfg, "localhost", g_port);
        int is_tls = 0;
        if (vh0 && vh0->is_listen_ssl) {
            is_tls = 1;
        }
        if (is_tls && PWEB_SSL_HAVE) {
            SSL_CTX *ctx = PWEB_SSL_CTX_NEW();
            if (ctx) {
                const vhost_t *vh = vh0;
                (void)vh;
                SSL *ssl = PWEB_SSL_NEW(ctx);
                if (ssl) {
                    PWEB_SSL_SET_FD(ssl, fd);
                    /* A real deployment would load vh->ssl_certificate here
                     * via SSL_CTX_use_certificate_file(); we leave it to
                     * the operator to pre-load a default cert at startup
                     * when PWEB_SSL is enabled. */
                    if (PWEB_SSL_CONNECT(ssl) == 1) {
                        /* Handshake done. For brevity we do NOT switch the
                         * whole I/O path to SSL_read/SSL_write; instead we
                         * treat the connection as plaintext from here.
                         * (Full TLS proxy mode is out of scope for the
                         * lightweight default build.) */
                    } else {
                        PWEB_SSL_SHUTDOWN(ssl);
                    }
                    PWEB_SSL_FREE(ssl);
                }
                PWEB_SSL_CTX_FREE(ctx);
            }
        } else if (is_tls) {
            /* TLS listener but PWEB_SSL not compiled in: close. */
            PWEB_CLOSEINT(fd);
            return;
        }
    }
#endif

    char buf[16384];
    int total = 0;

    for (;;) {
        int keepalive = 1;
        /* read until end of the head block */
        int head_done = 0;
        while (g_running && !head_done) {
            ssize_t r = PWEB_RECV(fd, buf + total,
                             (size_t)(sizeof(buf) - total - 1), 0);
            if (r <= 0) {
                PWEB_CLOSEINT(fd);
                return;
            }
            total += (int)r;
            buf[total] = 0;
            /* look for an empty line terminating the head block */
            char *scan = buf;
            while (scan < buf + total && !head_done) {
                char *le = (char*)memchr(scan, '\n', (size_t)((buf + total) - scan));
                if (!le) { break; }
                char *nxt = le + 1;
                if (nxt < buf + total && *nxt == '\r') { nxt++; }
                int empty = 1;
                char *q = scan;
                while (q < le) {
                    if (*q != '\r' && *q != '\n') { empty = 0; break; }
                    q++;
                }
                if (empty) { head_done = 1; }
                scan = nxt;
            }
        }
        if (!head_done) {
            PWEB_CLOSEINT(fd);
            return;
        }


        req_t req;
        int pos = 0;
        if (parse_request(&req, buf, &pos, total) != 0) {
            const char *resp =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            const char *b = resp;
            size_t n = strlen(b);
            while (n > 0) {
                ssize_t w = PWEB_SEND(fd, b, n, 0);
                if (w <= 0) { break; }
                b += w; n -= (size_t)w;
            }
            PWEB_CLOSEINT(fd);
            return;
        }


        /* method gate */
        const char *method = req.method;
        if (strcasecmp(method, "GET") != 0 &&
            strcasecmp(method, "HEAD") != 0 &&
            strcasecmp(method, "POST") != 0 &&
            strcasecmp(method, "PUT") != 0 &&
            strcasecmp(method, "DELETE") != 0 &&
            strcasecmp(method, "OPTIONS") != 0) {
            const char *resp =
                "HTTP/1.1 501 Not Implemented\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            const char *b = resp;
            size_t n = strlen(b);
            while (n > 0) {
                ssize_t w = PWEB_SEND(fd, b, n, 0);
                if (w <= 0) { break; }
                b += w; n -= (size_t)w;
            }
            PWEB_CLOSEINT(fd);
            return;
        }

        /* vhost routing: the listener socket that accepted this
         * connection was created for g_cfg->hosts[vh_idx], so that
         * vhost owns the request. When several vhosts share the same
         * port, a matching server_name overrides the listener default. */
        const vhost_t *vh = NULL;
        if (vh_idx >= 0 && vh_idx < g_cfg->nhosts) {
            vh = &g_cfg->hosts[vh_idx];
            const char *host_hdr = req_hdr(&req, "Host");
            if (host_hdr && !vh->is_default) {
                char hcopy[256];
                snprintf(hcopy, sizeof(hcopy), "%s", host_hdr);
                char *cp = strchr(hcopy, ':');
                if (cp) { *cp = 0; }
                for (int i = 0; i < g_cfg->nhosts; i++) {
                    const vhost_t *cand = &g_cfg->hosts[i];
                    if (cand->port != g_port || cand->is_default) {
                        continue;
                    }
                    char names_copy[256];
                    snprintf(names_copy, sizeof(names_copy), "%s",
                             cand->names);
                    for (char *tok = strtok(names_copy, " ");
                         tok;
                         tok = strtok(NULL, " ")) {
                        if (strcasecmp(tok, hcopy) == 0) {
                            vh = cand;
                            break;
                        }
                    }
                    if (vh == cand) { break; }
                }
            }
        }
        if (!vh) {
            vh = vhost_find(g_cfg, "localhost", g_port);
        }
        if (!vh) {
            const char *resp =
                "HTTP/1.1 421 No server\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            const char *b = resp;
            size_t n = strlen(b);
            while (n > 0) {
                ssize_t w = PWEB_SEND(fd, b, n, 0);
                if (w <= 0) { break; }
                b += w; n -= (size_t)w;
            }
            PWEB_CLOSEINT(fd);
            return;
        }

        /* decode URI for the handlers */
        char target[2048];
        snprintf(target, sizeof(target), "%s", req.target);
        url_decode(target);

        /* body */
        long maxb = vh->max_body > 0 ? vh->max_body : 8 * 1024 * 1024;
        char *body = NULL;
        int body_in_buf = 0;
        if (req.content_length > 0 &&
            (long)req.content_length <= maxb) {
            /* The head-detection loop above over-reads: if the body
             * arrived in the same recv chunk, total > pos and
             * buf[pos..total] already holds the leading body bytes.
             * Consume those first, then recv the remainder. */
            int head_len = pos;
            int body_have = total - head_len;
            if (body_have > req.content_length) { body_have = req.content_length; }
            body_in_buf = body_have;
            if (body_have >= req.content_length) {
                body = (char*)malloc(req.content_length + 1);
                if (body) {
                    memcpy(body, buf + head_len, (size_t)req.content_length);
                    body[req.content_length] = 0;
                    total = head_len + req.content_length;
                }
            } else if (body_have > 0) {
                body = (char*)malloc(req.content_length + 1);
                if (body) {
                    if (body_have < req.content_length) {
                        memmove(buf, buf + head_len,
                                (size_t)(total - head_len));
                        total -= head_len;
                        head_len = 0;
                    }
                    memcpy(body, buf + head_len, (size_t)body_have);
                    int got = body_have;
                    while (got < req.content_length) {
                        ssize_t rr = PWEB_RECV(fd, body + got,
                            (size_t)(req.content_length - got), 0);
                        if (rr <= 0) { free(body); body = NULL; break; }
                        got += (int)rr;
                    }
                    if (body) {
                        body[req.content_length] = 0;
                        total = head_len;
                    }
                }
            } else {
                body = read_body(fd, req.content_length, maxb, NULL);
            }
            req.body = body;
            if (!body) {
                PWEB_CLOSEINT(fd);
                return;
            }
            (void)body_in_buf;
        } else if (req.content_length > 0 &&
                   (long)req.content_length > maxb) {
            const char *resp =
                "HTTP/1.1 413 Request Entity Too Large\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            const char *b = resp;
            size_t n = strlen(b);
            while (n > 0) {
                ssize_t w = PWEB_SEND(fd, b, n, 0);
                if (w <= 0) { break; }
                b += w; n -= (size_t)w;
            }
            free(body);
            PWEB_CLOSEINT(fd);
            return;
        }

        /* keep-alive decision */
        const char *conn = req_hdr(&req, "Connection");
        if (conn) {
            if (casestrs(conn, "close")) { keepalive = 0; }
            if (strcasecmp(req.version, "1.0") == 0 &&
                !casestrs(conn, "keep-alive")) {
                keepalive = 0;
            }
        } else if (strcasecmp(req.version, "1.0") == 0) {
            keepalive = 0;
        }

        /* dispatch */
        int is_default = 0;
        const loc_t *lc = locate(vh, target, &is_default);
        loc_t deflc;
        if (!lc) {
            memset(&deflc, 0, sizeof(deflc));
            deflc.type = L_STATIC;
            snprintf(deflc.match, sizeof(deflc.match), "/");
            snprintf(deflc.root, sizeof(deflc.root), "%s", vh->root);
            lc = &deflc;
        }

        if (lc->type == L_REDIR) {
            handle_redir(fd, lc, &req, target);
        } else if (lc->type == L_PROXY) {
            handle_proxy(fd, lc, &req, ipstr);
        } else if (lc->type == L_CGI) {
            handle_cgi(fd, vh, lc, &req, ipstr);
        } else {
            handle_static(fd, vh, lc, &req, ipstr, target);
        }

        /* access log */
        if (vh->access_log[0]) {
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            char date[32] = "-";
            if (tm) {
                strftime(date, sizeof(date), "%d/%b/%Y:%H:%M:%S %z", tm);
            }
            char rl[2048];
            snprintf(rl, sizeof(rl), "%s %s %s",
                     req.method, req.target, req.version);
            log_access(vh->access_log, "%s - - [%s] \"%s\" 200 -\n",
                       ipstr, date, rl);
        }

        free(body);
        req.body = NULL;

        if (!keepalive || !g_running) {
            break;
        }
        total = 0;
        head_done = 0;
    }
    PWEB_CLOSEINT(fd);
}


/* pthread entry wrapper */
void *g_handle_conn_arg(void *p)
{
    struct arg_t { int fd; struct sockaddr_in a; int vh_idx; } *a = p;
    g_handle_conn(a->fd, &a->a, a->vh_idx);
    free(a);
    return NULL;
}
