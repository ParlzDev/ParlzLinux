/* config.c - nginx-like DSL parser + vhost/location build */
#include "pweb.h"
#include <ctype.h>

#include <stdio.h>
#include <strings.h>

/* ---------- config tree ---------- */
typedef struct cnode {
    char *key;
    char **vals;
    int nvals;
    struct cnode *child[32];
    int nchild;
} cnode;

static cnode *new_cnode(const char *key)
{
    cnode *n = (cnode*)calloc(1, sizeof(cnode));
    n->key = (char*)malloc(strlen(key) + 1);
    strcpy(n->key, key);
    n->nvals = 0;
    n->nchild = 0;
    return n;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end) {
        if (*p == '#') {
            const char *nl = strchr(p, '\n');
            p = nl ? nl + 1 : end;
            continue;
        }
        if (isspace((unsigned char)*p)) { p++; continue; }
        break;
    }
    return p;
}

static size_t read_token(const char **pp, const char *end, char *out,
                         size_t outsz)
{
    const char *p = *pp;
    p = skip_ws(p, end);
    if (p >= end) { *pp = p; return 0; }
    char *o = out;
    size_t n = 0;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (p < end && *p != q) {
            if (n + 1 < outsz) { o[n++] = *p; }
            p++;
        }
        if (p < end) { p++; }
    } else {
        while (p < end && !isspace((unsigned char)*p) &&
               *p != '{' && *p != '}' && *p != ';') {
            if (n + 1 < outsz) { o[n++] = *p; }
            p++;
        }
    }
    o[n] = 0;
    *pp = p;
    return n;
}

/* parse a sequence of directives/blocks inside 'parent'.
 * Stops when: (a) a closing '}' is found (consumed), or (b) end reached.
 * For top-level calls there is no enclosing '{' so '}' is not expected;
 * we still stop at '}' to avoid running off the end. */
static cnode *parse_seq(const char **pp, const char *end, cnode *parent,
                        int is_top)
{
    const char *p = *pp;
    p = skip_ws(p, end);
    while (p < end) {
        if (*p == '}') {
            p++;
            *pp = p;
            return parent;
        }
        char key[256];
        const char *prev = p;
        read_token(&p, end, key, sizeof(key));
        if (key[0] == 0 || p == prev) {
            /* cannot make progress: skip one byte and resync */
            p++;
            if (p >= end) { break; }
            continue;
        }
        cnode *n = new_cnode(key);
        p = skip_ws(p, end);
        if (p < end && *p == '{') {
            p++;
            parse_seq(&p, end, n, 0);
        } else {
            int cap = 0;
            int stopped_at_brace = 0;
            for (;;) {
                char v[512];
                size_t vl = read_token(&p, end, v, sizeof(v));
                if (vl == 0) { break; }
                /* store the token, THEN look for what follows */
                if (n->nvals == cap) {
                    cap = cap ? cap * 2 : 4;
                    n->vals = (char**)realloc(n->vals,
                                               (size_t)cap * sizeof(char*));
                }
                n->vals[n->nvals++] = (char*)malloc(vl + 1);
                memcpy(n->vals[n->nvals - 1], v, vl + 1);
                p = skip_ws(p, end);
                if (p < end && *p == '{') {
                    /* values followed by a block: "location /pat {" */
                    stopped_at_brace = 1;
                    break;
                }
                if (p < end && *p == ';') { p++; break; }
                if (p >= end) { break; }
            }
            if (stopped_at_brace && p < end && *p == '{') {
                p++;
                parse_seq(&p, end, n, 0);
            }
        }
        if (parent && parent->nchild < 32) {
            parent->child[parent->nchild++] = n;
        }
        p = skip_ws(p, end);
    }
    *pp = p;
    return parent;
}

static const char *child_val(const cnode *blk, const char *name, int idx,
                            const char *dflt)
{
    if (!blk) { return dflt; }
    for (int i = 0; i < blk->nchild; i++) {
        cnode *c = blk->child[i];
        if (c && c->nvals > idx && strcasecmp(c->key, name) == 0) {
            return c->vals[idx];
        }
    }
    return dflt;
}
static int child_flag(const cnode *blk, const char *name, int dflt)
{
    const char *v = child_val(blk, name, 0, NULL);
    return v ? (strcasecmp(v, "on") == 0) : dflt;
}
static int child_int(const cnode *blk, const char *name, int dflt)
{
    const char *v = child_val(blk, name, 0, NULL);
    return v ? atoi(v) : dflt;
}
static long size_val(const cnode *blk, const char *name, long dflt)
{
    const char *v = child_val(blk, name, 0, NULL);
    if (!v) { return dflt; }
    long num = strtol(v, (char **)&v, 10);
    long mult = 1;
    while (*v == ' ' || *v == '\t') { v++; }
    switch (*v) {
    case 'k': mult = 1024L; break;
    case 'm': mult = 1024L * 1024; break;
    case 'g': mult = 1024L * 1024 * 1024; break;
    default: break;
    }
    return num * mult;
}
int cfg_parse(const char *path, cfg_t **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "pweb: cannot open config %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0) {
        fclose(f);
        fprintf(stderr, "pweb: config %s is empty\n", path);
        return -1;
    }
    char *raw = (char*)malloc((size_t)flen + 1);
    if (!raw) { fclose(f); return -1; }
    size_t rd = fread(raw, 1, (size_t)flen, f);
    fclose(f);
    raw[rd] = 0;

    cnode *top = new_cnode("__top__");
    const char *p = raw;
    parse_seq(&p, raw + rd, top, 1);
    free(raw);

    cnode *http = NULL;
    for (int i = 0; i < top->nchild; i++) {
        if (strcasecmp(top->child[i]->key, "http") == 0) {
            http = top->child[i];
            break;
        }
    }
    if (!http) {
        fprintf(stderr, "pweb: config has no http block\n");
        return -1;
    }

    long def_max_body = size_val(http, "client_max_body_size",
                                 8 * 1024 * 1024);
    int def_keepalive = child_int(http, "keepalive_timeout", 75);
    int def_gzip = child_flag(http, "gzip", 1);
    int def_gzip_min = child_int(http, "gzip_min_length", 1024);

    int cap = 16;
    cfg_t *cfg = (cfg_t*)calloc(1, sizeof(cfg_t));
    cfg->hosts = (vhost_t*)calloc(cap, sizeof(vhost_t));
    int nh = 0;

    for (int i = 0; i < http->nchild; i++) {
        cnode *srv = http->child[i];
        if (strcasecmp(srv->key, "server") != 0) { continue; }
        if (nh >= cap) {
            cap *= 2;
            cfg->hosts = (vhost_t*)realloc(cfg->hosts,
                                           (size_t)cap * sizeof(vhost_t));
        }
        vhost_t *vh = &cfg->hosts[nh];
        memset(vh, 0, sizeof(*vh));
        vh->port = 80;
        vh->gzip = def_gzip;
        vh->gzip_min = def_gzip_min;
        vh->keepalive = def_keepalive;
        vh->max_body = def_max_body;
        snprintf(vh->access_log, sizeof(vh->access_log),
                 "./logs/access.log");
        snprintf(vh->error_log, sizeof(vh->error_log),
                 "./logs/error.log");

        char *root_dir = NULL;

        for (int j = 0; j < srv->nchild; j++) {
            cnode *e = srv->child[j];
            const char *kn = e->key;

            if (strcasecmp(kn, "location") == 0) {
                if (vh->nlocs == 0) {
                    vh->locs = (loc_t*)calloc(16, sizeof(loc_t));
                }
                char *lpat = (e->nvals > 0) ? e->vals[0] : "/";
                if (vh->nlocs >= 16) {
                    vh->locs = (loc_t*)realloc(vh->locs,
                        (size_t)(vh->nlocs + 4) * sizeof(loc_t));
                }
                loc_t *lc = &vh->locs[vh->nlocs];
                memset(lc, 0, sizeof(*lc));
                lc->type = L_STATIC;
                if (lpat[0] == '~') {
                    lc->is_regex = 1;
                    snprintf(lc->match, sizeof(lc->match), "%s", lpat + 1);
                } else if (lpat[0] == '=') {
                    const char *m = lpat + 1;
                    while (*m == ' ') { m++; }
                    lc->match[0] = 0;
                    lc->match[0] = '=';
                    strncat(lc->match, m, sizeof(lc->match) - 1);
                } else {
                    snprintf(lc->match, sizeof(lc->match), "%s", lpat);
                }
                if (root_dir) {
                    snprintf(lc->root, sizeof(lc->root), "%s", root_dir);
                }
                for (int d = 0; d < e->nchild; d++) {
                    cnode *dd = e->child[d];
                    if (!dd || dd->nvals == 0) { continue; }
                    const char *dkn = dd->key;
                    char *dv = dd->vals[0];
                    if (strcasecmp(dkn, "root") == 0) {
                        snprintf(lc->root, sizeof(lc->root), "%s", dv);
                    } else if (strcasecmp(dkn, "proxy_pass") == 0) {
                        lc->type = L_PROXY;
                        snprintf(lc->proxypass, sizeof(lc->proxypass),
                                 "%s", dv);
                    } else if (strcasecmp(dkn, "cgi_pass") == 0) {
                        /* cgi_pass <script>            -> script
                         * cgi_pass <interp> <script>   -> "interp script"
                         *   (nginx-style: first token is an interpreter,
                         *    e.g. `/bin/sh /path/to/shim.sh`).
                         * Join every value token with a space; handle_cgi
                         * splits on the first space to obtain interp +
                         * script_path. */
                        lc->type = L_CGI;
                        {
                            char joined[600] = "";
                            size_t jn = 0;
                            for (int vi = 0; vi < dd->nvals; vi++) {
                                const char *vv = dd->vals[vi];
                                if (vi > 0) {
                                    if (jn + 1 < sizeof(joined) - 1) {
                                        joined[jn++] = ' ';
                                        joined[jn] = 0;
                                    }
                                }
                                size_t vl = strlen(vv);
                                size_t room = sizeof(joined) - jn - 1;
                                if (vl > room) { vl = room; }
                                memcpy(joined + jn, vv, vl);
                                jn += vl;
                                joined[jn] = 0;
                            }
                            snprintf(lc->cgi_script, sizeof(lc->cgi_script),
                                     "%s", joined);
                        }
                    } else if (strcasecmp(dkn, "rewrite") == 0) {
                        lc->type = L_REDIR;
                        /* rewrite <regex> <replacement> [last|break|permanent] */
                        const char *repl = (dd->nvals > 1) ? dd->vals[1] : "";
                        const char *flag = (dd->nvals > 2) ? dd->vals[2] : "";
                        snprintf(lc->redir_pat, sizeof(lc->redir_pat),
                                 "%.255s", dv);
                        snprintf(lc->redir_repl, sizeof(lc->redir_repl),
                                 "%.255s", repl);
                        lc->redir_flag =
                            (strcasecmp(flag, "last") == 0 ||
                             strcasecmp(flag, "break") == 0) ? 1 : 0;
                        /* legacy combined field */
                        snprintf(lc->redir, sizeof(lc->redir), "%.511s %.511s",
                                 dv, repl);
                    }
                }
                vh->nlocs++;
                continue;
            }

            char *kv = e->nvals > 0 ? e->vals[0] : (char*)"";
            if (strcasecmp(kn, "listen") == 0) {
                int i2;
                int port = 0;
                int has_ssl = 0;
                for (i2 = 0; i2 < e->nvals; i2++) {
                    char *tok = e->vals[i2];
                    if (strcasecmp(tok, "ssl") == 0) { has_ssl = 1; continue; }
                    if (strcasecmp(tok, "default_server") == 0) {
                        vh->is_default = 1; continue;
                    }
                    /* port or addr:port or [::]:port */
                    const char *colon = strrchr(tok, ':');
                    if (colon && colon > tok) { port = atoi(colon + 1); }
                    else { port = atoi(tok); }
                }
                if (port > 0) { vh->port = port; }
                vh->is_listen_ssl = has_ssl;
            } else if (strcasecmp(kn, "ssl_certificate") == 0) {
                snprintf(vh->ssl_certificate, sizeof(vh->ssl_certificate),
                         "%s", kv);
            } else if (strcasecmp(kn, "server_name") == 0) {
                int off = 0;
                for (int v = 0; v < e->nvals && off < 250; v++) {
                    size_t l = strlen(e->vals[v]);
                    if (off > 0) { vh->names[off++] = ' '; }
                    if (l > 0 && off + (int)l < (int)sizeof(vh->names)) {
                        memcpy(vh->names + off, e->vals[v], l);
                        off += (int)l;
                    }
                }
                vh->names[off] = 0;
            } else if (strcasecmp(kn, "root") == 0) {
                free(root_dir);
                root_dir = (char*)malloc(strlen(kv) + 1);
                strcpy(root_dir, kv);
            
            } else if (strcasecmp(kn, "index") == 0) {
                int off = 0;
                for (int v = 0; v < e->nvals && off < 120; v++) {
                    size_t l = strlen(e->vals[v]);
                    if (off > 0) { vh->index_list[off++] = ' '; }
                    if (l > 0 && off + (int)l < (int)sizeof(vh->index_list)) {
                        memcpy(vh->index_list + off, e->vals[v], l);
                        off += (int)l;
                    }
                }
                vh->index_list[off] = 0;
            } else if (strcasecmp(kn, "autoindex") == 0) {
                vh->autoindex = (strcasecmp(kv, "on") == 0);
            } else if (strcasecmp(kn, "gzip") == 0) {
                vh->gzip = (strcasecmp(kv, "on") == 0);
            } else if (strcasecmp(kn, "gzip_min_length") == 0) {
                vh->gzip_min = atoi(kv);
            } else if (strcasecmp(kn, "access_log") == 0) {
                snprintf(vh->access_log, sizeof(vh->access_log), "%s", kv);
            } else if (strcasecmp(kn, "error_log") == 0) {
                snprintf(vh->error_log, sizeof(vh->error_log), "%s", kv);
            } else if (strcasecmp(kn, "client_max_body_size") == 0) {
                char *mm = (char*)kv;
                long num = strtol(mm, &mm, 10);
                long mult = 1;
                while (*mm == ' ' || *mm == '\t') { mm++; }
                switch (*mm) {
                case 'k': mult = 1024L; break;
                case 'm': mult = 1024L * 1024; break;
                case 'g': mult = 1024L * 1024 * 1024; break;
                default: break;
                }
                vh->max_body = num * mult;
            } else if (strcasecmp(kn, "keepalive_timeout") == 0) {
                vh->keepalive = atoi(kv);
            } else if (strcasecmp(kn, "auth_basic") == 0) {
                snprintf(vh->auth_realm, sizeof(vh->auth_realm), "%s", kv);
            } else if (strcasecmp(kn, "auth_user_file") == 0) {
                snprintf(vh->auth_file, sizeof(vh->auth_file), "%s", kv);
            }
        }
        if (vh->root[0] == 0 && root_dir) {
            snprintf(vh->root, sizeof(vh->root), "%s", root_dir);
        }
        free(root_dir);
        if (vh->index_list[0] == 0) {
            strcpy(vh->index_list, "index.html index.htm");
        }
        nh++;
    }
    cfg->nhosts = nh;
    *out = cfg;
    return 0;
}

void cfg_free(cfg_t *c)
{
    if (!c) { return; }
    for (int i = 0; i < c->nhosts; i++) {
        free(c->hosts[i].locs);
    }
    free(c->hosts);
    free(c);
}

const vhost_t *vhost_find(const cfg_t *c, const char *host, int port)
{
    if (!c) { return NULL; }
    /* Match on the listener port first: every connection arrives on a
     * specific socket, so that socket's vhost is the natural owner.
     * The Host header then selects among non-default vhosts sharing a
     * port; when no name matches, the default_server on that port wins,
     * finally any vhost on the port. */
    const vhost_t *default_vh = NULL;
    for (int i = 0; i < c->nhosts; i++) {
        const vhost_t *vh = &c->hosts[i];
        if (vh->port != port) { continue; }
        if (vh->is_default && !default_vh) { default_vh = vh; }
        if (!vh->is_default && vh->names[0]) {
            char host_copy[256];
            snprintf(host_copy, sizeof(host_copy), "%s", host);
            char *cp = strchr(host_copy, ':');
            if (cp) { *cp = 0; }
            char *names_copy = (char*)strdup(vh->names);
            if (names_copy) {
                for (char *tok = strtok(names_copy, " ");
                     tok;
                     tok = strtok(NULL, " ")) {
                    if (strcasecmp(tok, host_copy) == 0) {
                        free(names_copy);
                        return vh;
                    }
                }
                free(names_copy);
            }
        }
    }
    if (default_vh) { return default_vh; }
    /* last resort: any vhost on this port */
    for (int i = 0; i < c->nhosts; i++) {
        if (c->hosts[i].port == port) { return &c->hosts[i]; }
    }
    return NULL;
}
