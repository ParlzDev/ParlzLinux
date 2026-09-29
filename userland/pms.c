/*
 * pms.c - Parlz Mail Send/Read 命令行客户端(POSIX 移植版, 中文输出)
 *
 * 功能：
 *   list       列出收件箱（默认 imap.parlz.com:143，可 STARTTLS）
 *   read <N>   读取第 N 封邮件全文
 *   send       通过 SMTP 发信（默认 smtp.parlz.com:465，明文 AUTH）
 *   probe      测试服务器的连通性 + 横幅（无需账号）
 *
 * 技术栈：POSIX socket + getaddrinfo；可选 OpenSSL 静态库(TLS 升级
 *         STARTTLS/隐式 TLS，编译开关 PMS_HAVE_OPENSSL；未开则仅支持
 *         明文协议)。纯 C，无第三方运行时依赖(除可选 OpenSSL)。
 *
 * 服务器适配：自动检测「明文 / STARTTLS / 隐式 TLS」。
 *   连接后先看服务器是否主动发送明文横幅：
 *     - 有横幅  → 明文协议；若广告了 STARTTLS 且未关闭，则升级为加密。
 *     - 无横幅  → 隐式 TLS（先做 TLS 握手，再读横幅）。
 *  默认端口是实测可用的 143(IMAP 明文) 和 465(SMTP 明文 AUTH)。
 *
 * 服务器默认值(可用环境变量覆盖)：
 *   PMS_IMAP_HOST / PMS_IMAP_PORT / PMS_SMTP_HOST / PMS_SMTP_PORT
 *   PMS_USER / PMS_PASS          （缺省则交互输入）
 * 选项：
 *   --imap-tls <plain|starttls|implicit>
 *   --smtp-tls <plain|starttls|implicit>   (默认 plain：明文)
 *   --insecure                       跳过服务器证书校验(用于自签名证书)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <dirent.h>

#ifndef PMS_HAVE_OPENSSL
#define PMS_HAVE_OPENSSL 0
#endif
#if PMS_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#define IMAP_HOST "imap.parlz.com"
#define IMAP_PORT 143          /* 实测可用：Dovecot 明文 + STARTTLS */
#define SMTP_HOST "smtp.parlz.com"
#define SMTP_PORT 465          /* 实测可用：明文 SMTP，AUTH PLAIN/LOGIN */

typedef enum { TLS_PLAIN = 0, TLS_STARTTLS, TLS_IMPLICIT } TlsPref;

/* ------------------------------------------------------------------ */
/* 可配置项                                                            */
/* ------------------------------------------------------------------ */
static const char *g_imap_host = IMAP_HOST;
static int         g_imap_port = IMAP_PORT;
static const char *g_smtp_host = SMTP_HOST;
static int         g_smtp_port = SMTP_PORT;
static TlsPref     g_imap_tls  = TLS_PLAIN;  /* 该服务器 STARTTLS 损坏，默认明文 */
static TlsPref     g_smtp_tls  = TLS_PLAIN;  /* 该服务器 SMTP 为明文，无 STARTTLS */
static int         g_insecure  = 0;

static const char *g_user  = NULL;
static const char *g_pass  = NULL;
static const char *g_from  = NULL;
static const char *g_to    = NULL;
static const char *g_subj  = NULL;
static const char *g_body  = NULL;
static const char *g_bodyf = NULL;
static int         g_page   = 1;   /* list 分页页码 */
static int         g_html   = 0;   /* read --html 时把 HTML 正文存文件 */
static const char *g_files[32];    /* send 附件路径(--file / -F，可重复) */
static int         g_nfiles = 0;

/* ------------------------------------------------------------------ */
/* 通用小工具(POSIX：UTF-8 直接写, 无控制台宽字符转换)                  */
/* ------------------------------------------------------------------ */
static void out_utf8(const char *s, size_t n) {
    fwrite(s, 1, n, stdout);
    fflush(stdout);
}
static void err_utf8(const char *s, size_t n) {
    fwrite(s, 1, n, stderr);
    fflush(stderr);
}
static void out_str(const char *s) { out_utf8(s, strlen(s)); }

static void pms_cprintf(const char *fmt, ...) {
    char buf[8192]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    out_utf8(buf, strlen(buf));
}
static void cerr(const char *fmt, ...) {
    char buf[8192]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    err_utf8(buf, strlen(buf));
}
static void die(const char *fmt, ...) {
    char buf[8192], full[8224]; va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    snprintf(full, sizeof full, "错误：%s\n", buf);
    err_utf8(full, strlen(full));
    exit(1);
}
static int ci_eq_n(const char *a, const char *b, size_t n) {
    while (n--) {
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++)) return 0;
    }
    return 1;
}

typedef struct { char *data; size_t len, cap; } Buf;
static void buf_init(Buf *b) { b->cap = 128; b->len = 0; b->data = (char*)malloc(b->cap); if (!b->data) die("内存不足"); b->data[0] = 0; }
static void buf_append(Buf *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 128;
        while (nc < b->len + n + 1) nc *= 2;
        char *nd = (char*)realloc(b->data, nc); if (!nd) die("内存不足");
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, p, n); b->len += n; b->data[b->len] = 0;
}
static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
static void buf_puts(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

/* 读取整个文件到 buf，返回 0=成功 */
static int read_file(const char *path, Buf *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char chunk[8192]; size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(out, chunk, n);
    fclose(f);
    return 0;
}

static const char *base_name(const char *path) {
    const char *q = strrchr(path, '/');
    if (q && q[1]) return q + 1;
    return path;
}

static const char *mime_from_path(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (ci_eq_n(ext, ".pdf", 4)) return "application/pdf";
    if (ci_eq_n(ext, ".png", 4)) return "image/png";
    if (ci_eq_n(ext, ".jpg", 4) || ci_eq_n(ext, ".jpeg", 5)) return "image/jpeg";
    if (ci_eq_n(ext, ".gif", 4)) return "image/gif";
    if (ci_eq_n(ext, ".txt", 4) || ci_eq_n(ext, ".log", 4)) return "text/plain";
    if (ci_eq_n(ext, ".zip", 4)) return "application/zip";
    if (ci_eq_n(ext, ".doc", 4)) return "application/msword";
    if (ci_eq_n(ext, ".docx", 5)) return "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    if (ci_eq_n(ext, ".csv", 4)) return "text/csv";
    return "application/octet-stream";
}
/* base64 文本按 76 字符换行(符合 MIME 规范)后追加到 buf */
static void buf_append_b64(Buf *b, const char *b64) {
    size_t n = strlen(b64), i = 0;
    while (i < n) {
        size_t j = i + 76; if (j > n) j = n;
        buf_append(b, b64 + i, j - i);
        buf_append(b, "\r\n", 2);
        i = j;
    }
}

static void base64_encode(const char *in, size_t inlen, char *out, size_t outcap);

/* 把一段字节 base64 后(76 字符换行)追加到 buf */
static void buf_put_b64of(Buf *out, const char *data, size_t len) {
    size_t clen = len * 4 / 3 + 16;
    char *b64 = (char*)malloc(clen);
    if (!b64) die("内存不足");
    base64_encode(data, len, b64, clen);
    buf_append_b64(out, b64);
    free(b64);
}

/* 若字符串含非 ASCII，则用 RFC2047 编码成 =?utf-8?B?...?=，否则原样返回 */
static void encode_mime_word(const char *in, char *out, size_t cap) {
    int hi = 0; for (const char *p = in; *p; p++) if ((unsigned char)*p >= 0x80) { hi = 1; break; }
    if (!hi || !cap) { snprintf(out, cap, "%s", in); return; }
    size_t n = strlen(in);
    char *b = (char*)malloc(n * 2 + 4);
    if (!b) { snprintf(out, cap, "%s", in); return; }
    base64_encode(in, n, b, n * 2 + 4);
    snprintf(out, cap, "=?utf-8?B?%s?=", b);
    free(b);
}

static void base64_encode(const char *in, size_t inlen, char *out, size_t outcap) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0, i = 0;
    while (i + 3 <= inlen && o + 4 < outcap - 1) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2];
        out[o++] = t[(v >> 18) & 63]; out[o++] = t[(v >> 12) & 63];
        out[o++] = t[(v >> 6) & 63]; out[o++] = t[v & 63];
        i += 3;
    }
    size_t rem = inlen - i;
    if (rem == 1 && o + 4 < outcap - 1) {
        unsigned v = (unsigned char)in[i] << 16;
        out[o++] = t[(v >> 18) & 63]; out[o++] = t[(v >> 12) & 63]; out[o++] = '='; out[o++] = '=';
    } else if (rem == 2 && o + 4 < outcap - 1) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8);
        out[o++] = t[(v >> 18) & 63]; out[o++] = t[(v >> 12) & 63]; out[o++] = t[(v >> 6) & 63]; out[o++] = '=';
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ */
/* MIME / 编码解码(纯逻辑, 与平台无关)                                   */
/* ------------------------------------------------------------------ */
static void header_value(const char *blob, const char *name, char *out, size_t cap);

static size_t base64_decode(const char *in, size_t inlen, char *out, size_t outcap) {
    static int v[256]; static int init = 0;
    if (!init) {
        for (int i = 0; i < 256; i++) v[i] = -1;
        const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; t[i]; i++) v[(unsigned char)t[i]] = i;
        init = 1;
    }
    size_t o = 0; unsigned a = 0; int ab = 0;
    for (size_t i = 0; i < inlen && o + 1 < outcap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=') break;
        if (isspace(c)) continue;
        int d = v[c]; if (d < 0) continue;
        a = (a << 6) | (unsigned)d; ab += 6;
        if (ab >= 8) { ab -= 8; out[o++] = (char)((a >> ab) & 0xFF); }
    }
    out[o] = 0;
    return o;
}

/* quoted-printable 解码；word=1 时 '_'->空格(用于 RFC2047 Q 编码字) */
static size_t qp_decode(const char *in, size_t inlen, char *out, size_t outcap, int word) {
    size_t o = 0, i = 0;
    while (i < inlen && o + 1 < outcap) {
        unsigned char c = (unsigned char)in[i];
        if (c == '_' && word) { out[o++] = ' '; i++; continue; }
        if (c == '=' && i + 1 < inlen) {
            char h1 = in[i + 1];
            if (h1 == '\n') { i += 2; continue; }
            if (h1 == '\r' && i + 2 < inlen && in[i + 2] == '\n') { i += 3; continue; }
            if (isxdigit((unsigned char)h1) && i + 2 < inlen && isxdigit((unsigned char)in[i + 2])) {
                int hi = isdigit(h1) ? h1 - '0' : (toupper((unsigned char)h1) - 'A' + 10);
                int lo = isdigit(in[i + 2]) ? in[i + 2] - '0' : (toupper((unsigned char)in[i + 2]) - 'A' + 10);
                out[o++] = (char)((hi << 4) | lo); i += 3; continue;
            }
        }
        out[o++] = (char)c; i++;
    }
    out[o] = 0;
    return o;
}
/* ------------------------------------------------------------------ */
/* 字符集：POSIX 版把字节串直接按 UTF-8 处理(邮件正文本就是 UTF-8/
 *         US-ASCII)；对非 ASCII 的 ISO-8859-1/GBK 等历史字符集不做
 *         转码(直接原样追加)，终端按 UTF-8 显示可能乱码但不影响 ASCII。 */
/* 把字节串追加到 buf(UTF-8 直传) */
static void append_converted(Buf *out, const char *bytes, size_t len, const char *charset) {
    (void)charset;
    buf_append(out, bytes, len);
}

/* 解码 RFC2047 编码字 (?charset?B?...?= / ?charset?Q?...?=)，结果追加到 out */
static void decode_mime(Buf *out, const char *s) {
    const char *p = s ? s : "";
    while (*p) {
        if (p[0] == '=' && p[1] == '?') {
            const char *q1 = strchr(p + 2, '?');
            const char *q2 = q1 ? strchr(q1 + 1, '?') : NULL;
            const char *end = q2 ? strstr(q2 + 1, "?=") : NULL;
            if (q1 && q2 && end) {
                char charset[64]; size_t cl = (size_t)(q1 - (p + 2));
                if (cl >= sizeof charset) cl = sizeof charset - 1;
                memcpy(charset, p + 2, cl); charset[cl] = 0;
                char enc = q1[1];
                size_t dlen = (size_t)(end - (q2 + 1));
                char *data = (char*)malloc(dlen + 1); memcpy(data, q2 + 1, dlen); data[dlen] = 0;
                if (enc == 'B' || enc == 'b') {
                    char *dec = (char*)malloc(dlen + 3);
                    size_t dl = base64_decode(data, dlen, dec, dlen + 3);
                    append_converted(out, dec, dl, charset);
                    free(dec);
                } else {
                    char *dec = (char*)malloc(dlen + 3);
                    size_t dl = qp_decode(data, dlen, dec, dlen + 3, 1);
                    append_converted(out, dec, dl, charset);
                    free(dec);
                }
                free(data);
                p = end + 2;
                continue;
            }
        }
        buf_append(out, p, 1); p++;
    }
}

/* 从 "显示名 <地址>" 里提取可读发件人/收件人 */
static void pretty_from(const char *raw, char *out, size_t cap) {
    char addr[256] = ""; char disp[512] = "";
    const char *lt = strrchr(raw, '<');
    const char *gt = lt ? strchr(lt, '>') : NULL;
    if (lt && gt) {
        size_t al = (size_t)(gt - lt - 1);
        if (al >= sizeof addr) al = sizeof addr - 1;
        memcpy(addr, lt + 1, al); addr[al] = 0;
        size_t dl = (size_t)(lt - raw);
        while (dl > 0 && (raw[dl-1] == ' ' || raw[dl-1] == '\t' || raw[dl-1] == '"')) dl--;
        size_t st = 0; while (st < dl && (raw[st] == ' ' || raw[st] == '"')) st++;
        size_t n = dl - st; if (n >= sizeof disp) n = sizeof disp - 1;
        memcpy(disp, raw + st, n); disp[n] = 0;
    } else if (strchr(raw, '@')) {
        snprintf(addr, sizeof addr, "%s", raw);
    } else {
        snprintf(disp, sizeof disp, "%s", raw);
    }
    Buf db; buf_init(&db);
    decode_mime(&db, disp);
    if (db.len) {
        if (addr[0]) snprintf(out, cap, "%s <%s>", db.data, addr);
        else snprintf(out, cap, "%s", db.data);
    } else if (addr[0]) {
        snprintf(out, cap, "%s", addr);
    } else {
        snprintf(out, cap, "(未知)");
    }
    buf_free(&db);
}

/* 去除 HTML 标签，便于读取文本正文 */
static size_t strip_html(const char *in, char *out, size_t cap) {
    size_t o = 0; int in_tag = 0;
    for (const char *p = in; *p && o + 1 < cap; p++) {
        if (*p == '<') { in_tag = 1; continue; }
        if (*p == '>') { in_tag = 0; continue; }
        if (!in_tag) out[o++] = *p;
    }
    out[o] = 0;
    return o;
}

/* 从 Content-Type 里取 boundary */
static const char *find_boundary(const char *ctype, char *out, size_t cap) {
    const char *p = strstr(ctype, "boundary");
    if (!p) return NULL;
    p = strchr(p, '='); if (!p) return NULL; p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    if (*p == '"') { p++; while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++; }
    else while (*p && *p != ';' && *p != ' ' && *p != '\t' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return out;
}

/* 按传输编码解码一段正文 */
static char *decode_transfer(const char *cte, const char *body, size_t len) {
    if (cte && strstr(cte, "base64")) {
        Buf b; buf_init(&b);
        for (size_t i = 0; i < len; i++) if (!isspace((unsigned char)body[i])) buf_append(&b, body + i, 1);
        char *dec = (char*)malloc(b.len + 1);
        base64_decode(b.data, b.len, dec, b.len + 1);
        buf_free(&b);
        return dec;
    }
    if (cte && strstr(cte, "quoted-printable")) {
        char *dec = (char*)malloc(len + 1);
        qp_decode(body, len, dec, len + 1, 0);
        return dec;
    }
    char *dec = (char*)malloc(len + 1);
    memcpy(dec, body, len); dec[len] = 0;
    return dec;
}

/* 根据 Content-Type / 传输编码，把正文变成可读文本 */
static char *decode_body(const char *ctype, const char *cte, const char *body, size_t len) {
    Buf out; buf_init(&out);
    if (ctype && strstr(ctype, "multipart/")) {
        char boundary[256];
        if (find_boundary(ctype, boundary, sizeof boundary)) {
            char marker[300]; snprintf(marker, sizeof marker, "--%s", boundary);
            size_t ml = strlen(marker);
            int have_plain = 0;
            const char *p = strstr(body, marker);
            while (p) {
                const char *after = p + ml;
                if (after[0] == '-') break;                       /* "--boundary--" 结束标记 */
                if (after[0] == '\r' && after[1] == '\n') after += 2;
                else if (after[0] == '\n') after += 1;
                const char *next = strstr(after, marker);
                size_t plen = next ? (size_t)(next - after) : strlen(after);
                char *part = (char*)malloc(plen + 1);
                memcpy(part, after, plen); part[plen] = 0;
                const char *sep = strstr(part, "\r\n\r\n");
                char hdrs[1024] = "", pcte[64] = "", pctt[160] = "";
                const char *bp = ""; size_t bpl = 0;
                if (sep) {
                    size_t hl = (size_t)(sep - part); if (hl >= sizeof hdrs) hl = sizeof hdrs - 1;
                    memcpy(hdrs, part, hl); hdrs[hl] = 0;
                    bp = sep + 4; bpl = strlen(bp);
                } else {
                    size_t hl = strlen(part); if (hl >= sizeof hdrs) hl = sizeof hdrs - 1;
                    memcpy(hdrs, part, hl); hdrs[hl] = 0;
                }
                while (bpl > 0 && (bp[bpl-1] == '\n' || bp[bpl-1] == '\r')) bpl--;
                header_value(hdrs, "Content-Transfer-Encoding", pcte, sizeof pcte);
                header_value(hdrs, "Content-Type", pctt, sizeof pctt);
                if (!pctt[0]) strcpy(pctt, "text/plain");
                if (strstr(pctt, "text/plain")) {
                    char *dec = decode_transfer(pcte, bp, bpl);
                    buf_append(&out, dec, strlen(dec)); free(dec);
                    buf_append(&out, "\n", 1);
                    have_plain = 1;
                } else if (strstr(pctt, "text/html")) {
                    if (!have_plain) {
                        char *dec = decode_transfer(pcte, bp, bpl);
                        char *txt = (char*)malloc(strlen(dec) + 1);
                        strip_html(dec, txt, strlen(dec) + 1);
                        buf_append(&out, txt, strlen(txt)); free(txt); free(dec);
                        buf_append(&out, "\n", 1);
                    }
                }
                free(part);
                p = next;
            }
            return out.data;
        }
    }
    char *dec = decode_transfer(cte, body, len);
    if (ctype && strstr(ctype, "text/html")) {
        char *txt = (char*)malloc(strlen(dec) + 1);
        strip_html(dec, txt, strlen(dec) + 1);
        buf_append(&out, txt, strlen(txt)); free(txt);
    } else {
        buf_append(&out, dec, strlen(dec));
    }
    free(dec);
    return out.data;
}

static void imap_quote(char *out, size_t cap, const char *s) {
    size_t o = 0; if (o + 1 < cap) out[o++] = '"';
    for (const char *p = s; *p && o + 1 < cap; p++) {
        if (*p == '"' || *p == '\\') { if (o + 2 < cap) { out[o++] = '\\'; out[o++] = *p; } }
        else out[o++] = *p;
    }
    if (o + 1 < cap) out[o++] = '"';
    out[o] = 0;
}

static void rfc2822_date(char *out, size_t cap) {
    time_t now = time(NULL);
    struct tm *g = gmtime(&now);
    char z[64]; (void)strftime(z, sizeof z, "%a, %d %b %Y %H:%M:%S", g);
    snprintf(out, cap, "%s +0000", z);
}

/* 交互式读密码(不回显)：POSIX 用 termios 关回显逐字符读 */
static void prompt_secret(const char *label, char *out, size_t cap) {
    fputs(label, stdout); fflush(stdout);
    struct termios t_old, t_new;
    int has_tty = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &t_old) == 0;
    if (has_tty) {
        t_new = t_old;
        t_new.c_lflag &= ~(ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSANOW, &t_new);
    }
    size_t i = 0;
    for (;;) {
        unsigned char c;
        ssize_t r;
        if (has_tty) {
            r = read(STDIN_FILENO, &c, 1);
        } else {
            /* 非 tty：读一行 */
            static char ln[512]; static int ln_i = 0, ln_n = 0;
            if (ln_i >= ln_n) {
                ln_n = (int)fread(ln, 1, sizeof ln, stdin);
                ln_i = 0;
            }
            if (ln_i < ln_n) { r = 1; c = ln[ln_i++]; }
            else { r = 0; }
        }
        if (r <= 0) break;
        if (c == '\r' || c == '\n') break;
        if ((c == 8 || c == 127) && i > 0) i--;
        else if (i < cap - 1) out[i++] = (char)c;
    }
    if (has_tty) tcsetattr(STDIN_FILENO, TCSANOW, &t_old);
    out[i] = 0; putchar('\n');
}

/* 危险操作确认：输入 yes 继续、no 取消，乱输入最多 max 次机会，超次自动取消。返回 1=确认，0=取消 */
static int confirm(const char *what, int max) {
    for (int i = 1; i <= max; i++) {
        pms_cprintf("%s\n  请确认 - 输入 yes 继续 / no 取消 (第 %d/%d 次): ", what, i, max);
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof line, stdin)) { pms_cprintf("\n"); return 0; }
        line[strcspn(line, "\r\n")] = 0;
        if (ci_eq_n(line, "yes", 3) || !strcmp(line, "y")) return 1;
        if (ci_eq_n(line, "no", 2) || !strcmp(line, "n")) { pms_cprintf("  已取消。\n"); return 0; }
        pms_cprintf("  无法识别 “%s”，请只输入 yes 或 no。\n", line);
    }
    pms_cprintf("  超过 %d 次未正确确认，已取消。\n", max);
    return 0;
}
/* ------------------------------------------------------------------ */
/* TCP (POSIX)                                                        */
/* ------------------------------------------------------------------ */
static int net_connect(const char *host, int port) {
    struct addrinfo hints, *res = NULL, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    char ps[16]; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0) return -1;
    int s = -1;
    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0) continue;
        if (connect(s, p->ai_addr, p->ai_addrlen) == 0) break;
        close(s); s = -1;
    }
    freeaddrinfo(res);
    return s;
}

static int send_all(int s, const void *b, size_t n) {
    const char *p = (const char*)b;
    while (n) {
        int r = send(s, p, (n > 0x40000000u ? 0x40000000u : n), 0);
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}

/* 是否有可读数据（用于检测服务器是否主动发明文横幅）；1=可读 0=超时 -1=错误 */
static int peek_timeout(int s, unsigned ms) {
    fd_set r; FD_ZERO(&r); FD_SET(s, &r);
    struct timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (unsigned)(ms % 1000) * 1000;
    return select(s + 1, &r, NULL, NULL, &tv);
}

/* ------------------------------------------------------------------ */
/* TLS 客户端：OpenSSL(可选)。未编译 OpenSSL 时, 所有 TLS 路径不可用。   */
/* ------------------------------------------------------------------ */
typedef struct {
    int sock;
#if PMS_HAVE_OPENSSL
    SSL *ssl;
    SSL_CTX *ctx;
#endif
} TLS;

static void tls_init(TLS *t) { memset(t, 0, sizeof *t); t->sock = -1; }
static void tls_close(TLS *t) {
#if PMS_HAVE_OPENSSL
    if (t->ssl) { SSL_shutdown(t->ssl); SSL_free(t->ssl); t->ssl = NULL; }
    if (t->ctx) { SSL_CTX_free(t->ctx); t->ctx = NULL; }
#endif
    if (t->sock >= 0) close(t->sock);
    tls_init(t);
}

static int tls_prepare(TLS *t) {
#if PMS_HAVE_OPENSSL
    SSL_library_init();
    t->ctx = SSL_CTX_new(TLS_client_method());
    return t->ctx ? 0 : -1;
#else
    (void)t;
    return -1;   /* 无 OpenSSL: TLS 不可用 */
#endif
}

/* 对已连接 t->sock 做 TLS 握手; insecure 时跳过证书校验 */
static int tls_handshake(TLS *t, const char *host) {
#if PMS_HAVE_OPENSSL
    SSL *ssl = SSL_new(t->ctx);
    if (!ssl) return -1;
    if (SSL_set_fd(ssl, t->sock) != 1) { SSL_free(ssl); return -1; }
    if (!g_insecure)
        SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
    else
        SSL_set_verify(ssl, SSL_VERIFY_NONE, NULL);
    char hn[256]; snprintf(hn, sizeof hn, "%s", host);
    SSL_set_tlsext_host_name(ssl, hn);
    int rc = SSL_connect(ssl);
    if (rc != 1) { SSL_free(ssl); return -1; }
    t->ssl = ssl;
    return 0;
#else
    (void)t; (void)host;
    return -1;
#endif
}

static int tls_read(TLS *t, void *buf, size_t buflen) {
#if PMS_HAVE_OPENSSL
    if (!t->ssl) return -1;
    int r = SSL_read(t->ssl, buf, (int)buflen);
    return r <= 0 ? -1 : r;
#else
    (void)t; (void)buf; (void)buflen;
    return -1;
#endif
}

static int tls_write(TLS *t, const void *buf, size_t len) {
#if PMS_HAVE_OPENSSL
    if (!t->ssl) return -1;
    int off = 0;
    while ((size_t)off < len) {
        int r = SSL_write(t->ssl, (const char*)buf + off, (int)(len - (size_t)off));
        if (r <= 0) return -1;
        off += r;
    }
    return 0;
#else
    (void)t; (void)buf; (void)len;
    return -1;
#endif
}
/* ------------------------------------------------------------------ */
/* Stream：明文 recv 或 加密 tls_read 的统一字节流                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int sock;
    TLS *tls;              /* NULL = 明文 */
    char rbuf[8192];
    size_t rlen, rpos;
} Stream;

static void stream_init(Stream *s, int sock, TLS *t) { s->sock = sock; s->tls = t; s->rlen = s->rpos = 0; }

static int stream_fill(Stream *s) {
    if (s->rpos < s->rlen) return 1;
    int n;
    if (s->tls) n = tls_read(s->tls, s->rbuf, sizeof s->rbuf);
    else n = recv(s->sock, s->rbuf, sizeof s->rbuf, 0);
    if (n <= 0) return 0;
    s->rlen = (size_t)n; s->rpos = 0;
    return 1;
}
static int stream_read1(Stream *s, char *c) {
    if (!stream_fill(s)) return 0;
    *c = s->rbuf[s->rpos++];
    return 1;
}
static size_t stream_read(Stream *s, void *buf, size_t n) {
    size_t got = 0; char *p = (char*)buf;
    while (got < n) {
        if (s->rpos >= s->rlen) { if (!stream_fill(s)) break; }
        size_t take = (s->rlen - s->rpos) < (n - got) ? (s->rlen - s->rpos) : (n - got);
        memcpy(p + got, s->rbuf + s->rpos, take);
        s->rpos += take; got += take;
    }
    return got;
}
static int stream_write(Stream *s, const void *b, size_t n) {
    if (s->tls) return tls_write(s->tls, b, n);
    return send_all(s->sock, b, n);
}

/* ------------------------------------------------------------------ */
/* 协议连接：自动检测 明文/隐式TLS                                        */
/* ------------------------------------------------------------------ */
static int gen_read_line(Stream *s, char *out, size_t cap);

static int proto_connect(Stream *s, TLS *t, const char *host, int port, TlsPref pref, char *banner, size_t bcap) {
    if (tls_prepare(t) != 0) return -1;
    t->sock = net_connect(host, port);
    if (t->sock < 0) { tls_close(t); return -1; }
    stream_init(s, t->sock, NULL);

    if (pref == TLS_IMPLICIT) {
        /* 显式隐式 TLS：先握手、再读横幅 */
        if (!PMS_HAVE_OPENSSL) { pms_cprintf("隐式 TLS 需要 OpenSSL(本构建未启用)\n"); tls_close(t); return -1; }
        if (tls_handshake(t, host) != 0) { tls_close(t); return -1; }
        s->tls = t;
        if (gen_read_line(s, banner, bcap) < 0) { tls_close(t); return -1; }
        return 0;
    }
    /* 明文 / STARTTLS：等服务器主动发横幅，给足时间避免误判(修复偶发"连接失败") */
    int rd = peek_timeout(t->sock, 8000);
    if (rd <= 0) { tls_close(t); return -1; }
    if (gen_read_line(s, banner, bcap) < 0) { tls_close(t); return -1; }
    return 0;
}

static int gen_read_line(Stream *s, char *out, size_t cap) {
    size_t i = 0; char c;
    while (i + 1 < cap && stream_read1(s, &c)) {
        if (c == '\n') {
            if (i > 0 && out[i-1] == '\r') i--;
            out[i] = 0;
            return (int)i;
        }
        out[i++] = c;
    }
    out[i] = 0;
    return i > 0 ? (int)i : -1;
}

/* IMAP 逻辑行：若以字面量 {N} 结尾，返回前缀并给出 N(不消费字面量) */
static char *imap_line_v2(Stream *s, long *litlen) {
    *litlen = 0;
    Buf acc; buf_init(&acc);
    Buf seg; buf_init(&seg);
    char c; int got = 0;
    while (stream_read1(s, &c)) {
        if (c == '\n') { got = 1; break; }
        if (c == '\r') continue;
        buf_append(&seg, &c, 1);
    }
    if (!got) { buf_free(&seg); return acc.len ? acc.data : NULL; }

    size_t n = seg.len;
    if (n > 0 && seg.data[n-1] == '}') {
        size_t i = n - 1, k = i;
        while (k > 0 && isdigit((unsigned char)seg.data[k-1])) k--;
        if (k < i && k > 0 && seg.data[k-1] == '{') {
            char tmp[32]; size_t dlen = i - k;
            if (dlen < sizeof tmp) {
                memcpy(tmp, seg.data + k, dlen); tmp[dlen] = 0;
                long cnt = strtol(tmp, NULL, 10);
                seg.len = k - 1; seg.data[seg.len] = 0;
                buf_append(&acc, seg.data, seg.len);
                buf_free(&seg);
                *litlen = cnt;
                return acc.data;
            }
        }
    }
    buf_append(&acc, seg.data, seg.len);
    buf_free(&seg);
    return acc.data;
}

/* IMAP 逻辑行，把字面量内容展开进同一行（返回堆内存，调用方 free） */
static char *imap_line(Stream *s, size_t *outlen) {
    Buf acc; buf_init(&acc);
    for (;;) {
        long lit = 0;
        char *seg = imap_line_v2(s, &lit);
        if (!seg) { if (outlen) *outlen = acc.len; return acc.len ? acc.data : NULL; }
        if (lit > 0) {
            buf_append(&acc, seg, strlen(seg)); free(seg);
            char *litbuf = (char*)malloc((size_t)lit + 1);
            if (!litbuf) die("内存不足");
            size_t got = stream_read(s, litbuf, (size_t)lit);
            litbuf[got] = 0;
            buf_append(&acc, litbuf, got);
            free(litbuf);
            continue;
        } else {
            buf_append(&acc, seg, strlen(seg)); free(seg);
            if (outlen) *outlen = acc.len;
            return acc.data;
        }
    }
}
/* ------------------------------------------------------------------ */
/* IMAP                                                                */
/* ------------------------------------------------------------------ */
static unsigned g_tag = 0;

static void imap_send(Stream *s, const char *tag, const char *fmt, ...) {
    char cmd[2048]; va_list ap; va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap);
    char full[2300];
    snprintf(full, sizeof full, "%s %s\r\n", tag, cmd);
    stream_write(s, full, strlen(full));
}

static int imap_is_tagged(const char *line, const char *tag) {
    size_t n = strlen(tag);
    return strncmp(line, tag, n) == 0 && line[n] == ' ';
}

static int imap_wait_ok(Stream *s, const char *tag) {
    for (;;) {
        char *line = imap_line(s, NULL);
        if (!line) return -1;
        if (imap_is_tagged(line, tag)) {
            int ok = ci_eq_n(line + strlen(tag) + 1, "OK", 2);
            free(line);
            return ok ? 0 : -1;
        }
        free(line);
    }
}

static int imap_starttls(Stream *s, TLS *t, const char *host) {
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    imap_send(s, tag, "%s", "STARTTLS");
    if (imap_wait_ok(s, tag) != 0) return -1;
    if (!PMS_HAVE_OPENSSL) { cerr("本构建未启用 OpenSSL，无法做 STARTTLS 加密\n"); return -1; }
    if (tls_handshake(t, host) != 0) return -1;
    s->tls = t;
    char *g = imap_line(s, NULL); if (!g) return -1; free(g);
    return 0;
}

static int imap_prepare(Stream *s, TLS *t) {
    char banner[1024];
    if (proto_connect(s, t, g_imap_host, g_imap_port, g_imap_tls, banner, sizeof banner) != 0)
        return -1;
    if (s->tls == NULL && g_imap_tls == TLS_STARTTLS) {
        if (strstr(banner, "STARTTLS")) {
            if (imap_starttls(s, t, g_imap_host) != 0) return -1;
        } else {
            cerr("IMAP 服务器不支持 STARTTLS(加密)\n");
            return -1;
        }
    }
    return 0;
}

static int imap_login(Stream *s, TLS *t, const char *user, const char *pass) {
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    char uq[512], pq[512], cmd[1200];
    imap_quote(uq, sizeof uq, user);
    imap_quote(pq, sizeof pq, pass);
    snprintf(cmd, sizeof cmd, "LOGIN %s %s", uq, pq);
    imap_send(s, tag, "%s", cmd);
    return imap_wait_ok(s, tag);
}

static int imap_select(Stream *s, TLS *t, int *exists, const char *mbox) {
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    char cmd[64]; snprintf(cmd, sizeof cmd, "SELECT %s", mbox);
    imap_send(s, tag, "%s", cmd);
    *exists = -1;
    for (;;) {
        char *line = imap_line(s, NULL);
        if (!line) return -1;
        if (imap_is_tagged(line, tag)) {
            int ok = ci_eq_n(line + strlen(tag) + 1, "OK", 2);
            free(line);
            return ok ? 0 : -1;
        }
        if (line[0] == '*') {
            int n = -1, used = 0;
            /* 必须整行是 "* N EXISTS" 才算消息数；否则会被 "* 0 RECENT" 之类误覆盖 */
            if (sscanf(line, "* %d %n", &n, &used) == 1) {
                const char *rest = line + used;
                while (*rest == ' ' || *rest == '\t') rest++;
                if (ci_eq_n(rest, "EXISTS", 6) && (rest[6] == ' ' || rest[6] == '\0')) {
                    if (n >= 0) *exists = n;
                }
            }
        }
        free(line);
    }
}

static void header_value(const char *blob, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    /* 在字段边界处匹配 "Name:(值)"：
       - 允许紧跟 FETCH 前缀后的第一个字段(前面是空格)；
       - 但跳过嵌入在其它值里的 "Name:"(如 DKIM 的 h=From:To:Subject:Date;) */
    for (const char *p = blob; *p; p++) {
        if (p != blob) {
            unsigned char pc = (unsigned char)p[-1];
            if (isalnum(pc) || pc == ':' || pc == '=' || pc == '.' || pc == '-' ||
                pc == '_' || pc == '@' || pc == '<' || pc == '>' || pc == '+' || pc == '/')
                continue;
        }
        if (ci_eq_n(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            size_t o = 0;
            /* 处理折叠头字段：后续以空格/tab 开头的行是续行，展开到同一值 */
            for (;;) {
                while (*v && *v != '\r' && *v != '\n' && o + 1 < cap) out[o++] = *v++;
                if (*v == '\r' && v[1] == '\n') v += 2;
                else if (*v == '\n') v += 1;
                else break;
                if (*v == ' ' || *v == '\t') {
                    if (o + 1 < cap) out[o++] = ' ';
                    while (*v == ' ' || *v == '\t') v++;
                    continue;
                }
                break;
            }
            out[o] = 0;
            return;
        }
    }
    out[0] = 0;
}

#define PAGE_SIZE 5

typedef struct { int no; char date[96]; char from[512]; char subj[1024]; } MsgEntry;

/* FETCH <msgset> 头字段 → 解析到 arr，按消息序号倒序(最新在前)，返回条数 */
static int fetch_entries(Stream *s, TLS *t, const char *msgset, MsgEntry *arr, int max) {
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    char cmd[420];
    snprintf(cmd, sizeof cmd, "FETCH %s (INTERNALDATE BODY.PEEK[HEADER.FIELDS (SUBJECT FROM DATE)])", msgset);
    imap_send(s, tag, "%s", cmd);
    int cnt = 0;
    for (;;) {
        char *line = imap_line(s, NULL);
        if (!line) break;
        if (imap_is_tagged(line, tag)) {
            int ok = ci_eq_n(line + strlen(tag) + 1, "OK", 2);
            free(line);
            if (!ok) die("读取邮件(FETCH)失败");
            break;
        }
        if (line[0] == '*') {
            int no = -1;
            if (sscanf(line, "* %d FETCH", &no) == 1 && no >= 1 && cnt < max) {
                MsgEntry *e = &arr[cnt++];
                e->no = no; e->date[0] = 0; e->from[0] = 0; e->subj[0] = 0;
                const char *d = strstr(line, "INTERNALDATE");
                if (d) {
                    d += strlen("INTERNALDATE");
                    while (*d == ' ') d++;
                    if (*d == '"') { d++; size_t j = 0; while (d[j] && d[j] != '"' && j + 1 < sizeof e->date) { e->date[j] = d[j]; j++; } e->date[j] = 0; }
                }
                header_value(line, "From", e->from, sizeof e->from);
                header_value(line, "Subject", e->subj, sizeof e->subj);
            }
        }
        free(line);
    }
    for (int i = 1; i < cnt; i++) {           /* 消息序号从大到小 = 最新在前 */
        MsgEntry key = arr[i]; int j = i - 1;
        while (j >= 0 && arr[j].no < key.no) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = key;
    }
    (void)t;
    return cnt;
}

static void print_entries(MsgEntry *arr, int cnt) {
    for (int i = 0; i < cnt; i++) {
        char pfrom[600]; pretty_from(arr[i].from, pfrom, sizeof pfrom);
        Buf sb; buf_init(&sb); decode_mime(&sb, arr[i].subj);
        pms_cprintf("%5d.  %s\n      发件人: %s\n      主题:   %s\n", arr[i].no, arr[i].date, pfrom, sb.len ? sb.data : "(无主题)");
        buf_free(&sb);
    }
}

static void imap_list(Stream *s, TLS *t, int exists, int page) {
    if (exists <= 0) { pms_cprintf("(该文件夹为空)\n"); return; }
    if (page < 1) page = 1;
    int total_pages = (exists + PAGE_SIZE - 1) / PAGE_SIZE;
    if (page > total_pages) { pms_cprintf("第 %d 页没有邮件(共 %d 页)\n", page, total_pages); return; }
    int hi = exists - (page - 1) * PAGE_SIZE;   /* 本页最新(序号最大) */
    int lo = hi - PAGE_SIZE + 1; if (lo < 1) lo = 1;
    char rng[32]; snprintf(rng, sizeof rng, "%d:%d", lo, hi);
    MsgEntry arr[PAGE_SIZE + 1];
    int cnt = fetch_entries(s, t, rng, arr, PAGE_SIZE);
    print_entries(arr, cnt);
    pms_cprintf("-- 共 %d 封邮件 · 第 %d/%d 页 --\n", exists, page, total_pages);
    if (page < total_pages) pms_cprintf("查看下一页: pms list %d\n", page + 1);
    else                     pms_cprintf("已经是最后一页。\n");
    (void)t;
}
static void open_html(const char *msg, size_t len);

/* 把整封报文渲染成可读形式 */
static void render_message(const char *msg, size_t len) {
    char hdrs[8192] = "";
    const char *body = ""; size_t blen = 0;
    const char *sep = strstr(msg, "\r\n\r\n");
    if (sep) {
        size_t hl = (size_t)(sep - msg); if (hl >= sizeof hdrs) hl = sizeof hdrs - 1;
        memcpy(hdrs, msg, hl); hdrs[hl] = 0;
        body = sep + 4; blen = strlen(body);
    } else {
        size_t hl = len < sizeof hdrs ? len : sizeof hdrs - 1;
        memcpy(hdrs, msg, hl); hdrs[hl] = 0;
    }
    char subj[1024] = "", from[1024] = "", to_[1024] = "", date[256] = "", cte[64] = "", ctt[160] = "";
    header_value(hdrs, "Subject", subj, sizeof subj);
    header_value(hdrs, "From", from, sizeof from);
    header_value(hdrs, "To", to_, sizeof to_);
    header_value(hdrs, "Date", date, sizeof date);
    header_value(hdrs, "Content-Transfer-Encoding", cte, sizeof cte);
    header_value(hdrs, "Content-Type", ctt, sizeof ctt);
    if (!ctt[0]) strcpy(ctt, "text/plain");
    Buf sb; buf_init(&sb); decode_mime(&sb, subj);
    char pfrom[600], pto[600];
    pretty_from(from, pfrom, sizeof pfrom);
    pretty_from(to_, pto, sizeof pto);
    char *btxt = decode_body(ctt, cte, body, blen);
    pms_cprintf("主题: %s\n发件人: %s\n收件人: %s\n日期: %s\n\n%s\n",
                sb.len ? sb.data : "(无主题)", pfrom, pto, date[0] ? date : "(未知)",
                btxt ? btxt : "(无正文)");
    free(btxt);
    buf_free(&sb);
}

/* 取第 msgnum 封的整封原始邮件到 out，返回 0=成功 */
static int imap_fetch_msg(Stream *s, TLS *t, int msgnum, Buf *out) {
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    char cmd[64]; snprintf(cmd, sizeof cmd, "FETCH %d BODY.PEEK[]", msgnum);
    imap_send(s, tag, "%s", cmd);
    int got_any = 0, res = -1;
    for (;;) {
        long lit = 0;
        char *line = imap_line_v2(s, &lit);
        if (!line) break;
        if (imap_is_tagged(line, tag)) {
            int ok = ci_eq_n(line + strlen(tag) + 1, "OK", 2);
            free(line);
            res = ok ? 0 : -1;
            break;
        }
        if (lit > 0) {
            char *chunk = (char*)malloc((size_t)lit);
            if (!chunk) die("内存不足");
            size_t got = stream_read(s, chunk, (size_t)lit);
            buf_append(out, chunk, got);
            free(chunk);
            got_any = 1;
        }
        free(line);
    }
    (void)t;
    return (res == 0 && got_any) ? 0 : res;
}

static int imap_read(Stream *s, TLS *t, int msgnum, int exists) {
    if (msgnum < 1 || msgnum > exists) {
        cerr("无效的邮件序号 %d(范围 1..%d)\n", msgnum, exists);
        return 1;
    }
    Buf msg; buf_init(&msg);
    if (imap_fetch_msg(s, t, msgnum, &msg) != 0) { buf_free(&msg); return -1; }
    if (g_html) open_html(msg.data, msg.len);
    else        render_message(msg.data, msg.len);
    buf_free(&msg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* IMAP 操作(标记/删除/移动/搜索/存档)                                    */
/* ------------------------------------------------------------------ */
static int imap_run(Stream *s, const char *fmt, ...) {
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    char cmd[512]; va_list ap; va_start(ap, fmt); vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap);
    imap_send(s, tag, "%s", cmd);
    return imap_wait_ok(s, tag);
}

static void imap_open_inbox(Stream *s, TLS *t, int *exists) {
    if (imap_prepare(s, t) != 0) die("IMAP 连接失败(可尝试加 --insecure 或检查主机/端口)");
    if (imap_login(s, t, g_user, g_pass) != 0) die("IMAP 登录失败(账号或密码错误)");
    if (imap_select(s, t, exists, "INBOX") != 0) die("打开收件箱(SELECT INBOX)失败");
}

/* 把字节流规范化成 CRLF 行尾 */
static void to_crlf(const char *src, size_t len, Buf *out) {
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\r') { buf_append(out, "\r\n", 2); if (i + 1 < len && src[i+1] == '\n') i++; }
        else if (c == '\n') buf_append(out, "\r\n", 2);
        else buf_append(out, &c, 1);
    }
}

/* APPEND 到指定文件夹，返回 0=成功 */
static int imap_append(Stream *s, TLS *t, const char *mbox, const char *msg, size_t len) {
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    char cmd[256]; snprintf(cmd, sizeof cmd, "APPEND %s (\\Seen) {%llu}", mbox, (unsigned long long)len);
    imap_send(s, tag, "%s", cmd);
    for (;;) {                                   /* 等待 "+" 续读字面量 */
        char *line = imap_line(s, NULL);
        if (!line) return -1;
        if (line[0] == '+') { free(line); break; }
        if (imap_is_tagged(line, tag)) { free(line); return -1; }
        free(line);
    }
    if (stream_write(s, msg, len) != 0) return -1;
    if (stream_write(s, "\r\n", 2) != 0) return -1;
    return imap_wait_ok(s, tag);
}

/* 发信成功后，把邮件存一份到已发送(Sent) */
static void save_to_sent(const char *msg, size_t len) {
    if (!g_user || !g_pass) return;
    TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
    if (imap_prepare(&s, &t) != 0 || imap_login(&s, &t, g_user, g_pass) != 0) {
        cerr("提示：发送成功，但没能连接 IMAP 存档到已发送(Sent)\n");
        tls_close(&t); return;
    }
    int ok = imap_append(&s, &t, "Sent", msg, len);
    tls_close(&t);
    if (ok != 0) cerr("提示：发送成功，但存档到 Sent 失败(可能没有名为 Sent 的文件夹)\n");
    else        pms_cprintf("已在已发送(Sent)存档一份。\n");
}

/* 搜索：SEARCH OR SUBJECT/TEXT，返回消息序号并列出(可直接 read) */
static void imap_search(Stream *s, TLS *t, const char *kw) {
    char kwq[256]; imap_quote(kwq, sizeof kwq, kw);
    char tag[16]; snprintf(tag, sizeof tag, "a%03u", ++g_tag);
    char cmd[560]; snprintf(cmd, sizeof cmd, "SEARCH OR SUBJECT %s TEXT %s", kwq, kwq);
    imap_send(s, tag, "%s", cmd);
    int nums[300]; int cnt = 0;
    for (;;) {
        char *line = imap_line(s, NULL);
        if (!line) break;
        if (imap_is_tagged(line, tag)) {
            int ok = ci_eq_n(line + strlen(tag) + 1, "OK", 2);
            free(line);
            if (!ok) die("搜索失败");
            break;
        }
        if (line[0] == '*' && strstr(line, "SEARCH")) {
            const char *p = strstr(line, "SEARCH") + 6;
            while (*p) {
                while (*p == ' ') p++;
                if (!*p) break;
                char *end; long v = strtol(p, &end, 10);
                if (end == p) break;
                if (cnt < 300) nums[cnt++] = (int)v;
                p = end;
            }
        }
        free(line);
    }
    if (!cnt) { pms_cprintf("未找到包含 “%s” 的邮件\n", kw); return; }
    char msgset[8192]; size_t off = 0; msgset[0] = 0;
    for (int i = 0; i < cnt && off < sizeof msgset - 2; i++)
        off += (size_t)snprintf(msgset + off, sizeof msgset - off, "%s%d", i ? "," : "", nums[i]);
    MsgEntry arr[300];
    int n = fetch_entries(s, t, msgset, arr, 300);
    pms_cprintf("搜索 “%s” 共找到 %d 封：\n", kw, n);
    print_entries(arr, n);
    if (n) pms_cprintf("用 pms read <序号> 看全文；pms seen/unseen/delete/move 处理邮件。\n");
}

/* 从邮件里提取 text/html 部分(不剥标签)，无则返回 NULL */
static char *extract_html(const char *ctype, const char *cte, const char *body, size_t len) {
    if (ctype && strstr(ctype, "multipart/")) {
        char boundary[256];
        if (find_boundary(ctype, boundary, sizeof boundary)) {
            char marker[300]; snprintf(marker, sizeof marker, "--%s", boundary);
            size_t ml = strlen(marker);
            const char *p = strstr(body, marker);
            char *result = NULL;
            while (p) {
                const char *after = p + ml;
                if (after[0] == '-') break;
                if (after[0] == '\r' && after[1] == '\n') after += 2;
                else if (after[0] == '\n') after += 1;
                const char *next = strstr(after, marker);
                size_t plen = next ? (size_t)(next - after) : strlen(after);
                char *part = (char*)malloc(plen + 1);
                memcpy(part, after, plen); part[plen] = 0;
                const char *sep = strstr(part, "\r\n\r\n");
                char hdrs[1024] = "", pctt[160] = "", pcte[64] = "";
                const char *bp = ""; size_t bpl = 0;
                if (sep) {
                    size_t hl = (size_t)(sep - part); if (hl >= sizeof hdrs) hl = sizeof hdrs - 1;
                    memcpy(hdrs, part, hl); hdrs[hl] = 0;
                    bp = sep + 4; bpl = strlen(bp);
                } else {
                    size_t hl = strlen(part); if (hl >= sizeof hdrs) hl = sizeof hdrs - 1;
                    memcpy(hdrs, part, hl); hdrs[hl] = 0;
                }
                while (bpl > 0 && (bp[bpl-1] == '\n' || bp[bpl-1] == '\r')) bpl--;
                header_value(hdrs, "Content-Type", pctt, sizeof pctt);
                header_value(hdrs, "Content-Transfer-Encoding", pcte, sizeof pcte);
                if (strstr(pctt, "text/html")) { result = decode_transfer(pcte, bp, bpl); free(part); break; }
                free(part);
                p = next;
            }
            return result;
        }
    }
    if (ctype && strstr(ctype, "text/html")) return decode_transfer(cte, body, len);
    return NULL;
}

/* 把邮件正文写成临时 HTML 文件(POSIX 无 ShellExecute；只存文件, 供 w3m 打开) */
static void open_html(const char *msg, size_t len) {
    char hdrs[8192] = ""; const char *body = ""; size_t blen = 0;
    const char *sep = strstr(msg, "\r\n\r\n");
    if (sep) {
        size_t hl = (size_t)(sep - msg); if (hl >= sizeof hdrs) hl = sizeof hdrs - 1;
        memcpy(hdrs, msg, hl); hdrs[hl] = 0;
        body = sep + 4; blen = strlen(body);
    } else {
        size_t hl = len < sizeof hdrs ? len : sizeof hdrs - 1;
        memcpy(hdrs, msg, hl); hdrs[hl] = 0;
    }
    char cte[64] = "", ctt[160] = "";
    header_value(hdrs, "Content-Transfer-Encoding", cte, sizeof cte);
    header_value(hdrs, "Content-Type", ctt, sizeof ctt);
    if (!ctt[0]) strcpy(ctt, "text/plain");
    char *html = extract_html(ctt, cte, body, blen);
    char tmp[512];
    const char *tmpdir = getenv("TMP"); if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmp, sizeof tmp, "%s/pms_%u.html", tmpdir, (unsigned)time(NULL));
    FILE *f = fopen(tmp, "wb");
    if (!f) { pms_cprintf("HTML 正文无法写入临时文件: %s\n", tmp); free(html); return; }
    if (html) {
        fwrite(html, 1, strlen(html), f);
    } else {
        char *plain = decode_body(ctt, cte, body, blen);
        fputs("<html><head><meta charset=\"utf-8\"></head><body><pre>", f);
        for (const char *q = plain; q && *q; q++) {
            if (*q == '&') fputs("&amp;", f);
            else if (*q == '<') fputs("&lt;", f);
            else if (*q == '>') fputs("&gt;", f);
            else fputc(*q, f);
        }
        fputs("</pre></body></html>", f);
        free(plain);
    }
    fclose(f);
    free(html);
    pms_cprintf("HTML 正文已保存到: %s\n  可用 w3m %s 查看\n", tmp, tmp);
}

/* 大小写不敏感的子串搜索 */
static const char *ci_strstr(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; p && *p; p++) if (ci_eq_n(p, needle, nl)) return p;
    return NULL;
}

/* 从头字段里取参数值，如 filename="x.pdf" / name=report.pdf */
static void param_value(const char *hdr, const char *param, char *out, size_t cap) {
    size_t pl = strlen(param);
    const char *p = ci_strstr(hdr, param);
    if (p && p[pl] == '=') {
        p += pl + 1;
        while (*p == ' ' || *p == '\t') p++;
        size_t o = 0;
        if (*p == '"') { p++; while (*p && *p != '"' && o + 1 < cap) out[o++] = *p++; out[o] = 0; return; }
        while (*p && *p != ';' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && o + 1 < cap) out[o++] = *p++;
        out[o] = 0;
        return;
    }
    out[0] = 0;
}

static int is_attachment(const char *ctype, const char *cd) {
    if (cd && (ci_strstr(cd, "attachment") || ci_strstr(cd, "filename"))) return 1;
    if (ctype && ci_strstr(ctype, "name=")) return 1;
    return 0;
}

/* 取附件文件名(解码 RFC2047 并做路径净化)，无则用 attachment_N.bin */
static void part_filename(const char *ctype, const char *cd, int idx, char *out, size_t cap) {
    char raw[512] = "";
    if (cd && ci_strstr(cd, "filename=")) param_value(cd, "filename", raw, sizeof raw);
    if (!raw[0] && ctype) param_value(ctype, "name", raw, sizeof raw);
    if (!raw[0]) { snprintf(out, cap, "attachment_%d.bin", idx); return; }
    Buf db; buf_init(&db); decode_mime(&db, raw);
    const char *src = db.len ? db.data : raw;
    size_t o = 0;
    for (const char *q = src; *q && o + 1 < cap; q++) {
        if (*q == '\\' || *q == '/' || *q == ':') out[o++] = '_'; else out[o++] = *q;
    }
    out[o] = 0;
    buf_free(&db);
}

/* 遍历实体，保存属于附件的部分 */
static void walk_entity(const char *ctype, const char *cte, const char *cd,
                        const char *body, size_t blen, const char *dir, int *idx, int *saved) {
    if (ctype && strstr(ctype, "multipart/")) {
        char boundary[256];
        if (find_boundary(ctype, boundary, sizeof boundary)) {
            char marker[300]; snprintf(marker, sizeof marker, "--%s", boundary);
            size_t ml = strlen(marker);
            const char *p = strstr(body, marker);
            while (p) {
                const char *after = p + ml;
                if (after[0] == '-') break;
                if (after[0] == '\r' && after[1] == '\n') after += 2; else if (after[0] == '\n') after += 1;
                const char *next = strstr(after, marker);
                size_t plen = next ? (size_t)(next - after) : strlen(after);
                char *part = (char*)malloc(plen + 1); memcpy(part, after, plen); part[plen] = 0;
                const char *sep = strstr(part, "\r\n\r\n");
                char ph[1024] = "", pctt[160] = "", pcte[64] = "", pcd[256] = "";
                const char *bp = ""; size_t bpl = 0;
                if (sep) { size_t hl = (size_t)(sep - part); if (hl >= sizeof ph) hl = sizeof ph - 1; memcpy(ph, part, hl); ph[hl] = 0; bp = sep + 4; bpl = strlen(bp); }
                else { size_t hl = strlen(part); if (hl >= sizeof ph) hl = sizeof ph - 1; memcpy(ph, part, hl); ph[hl] = 0; }
                while (bpl > 0 && (bp[bpl-1] == '\n' || bp[bpl-1] == '\r')) bpl--;
                header_value(ph, "Content-Type", pctt, sizeof pctt);
                header_value(ph, "Content-Transfer-Encoding", pcte, sizeof pcte);
                header_value(ph, "Content-Disposition", pcd, sizeof pcd);
                walk_entity(pctt, pcte, pcd, bp, bpl, dir, idx, saved);
                free(part);
                p = next;
            }
        }
    } else if (is_attachment(ctype, cd)) {
        (*idx)++;
        char fname[512]; part_filename(ctype, cd, *idx, fname, sizeof fname);
        char *data = decode_transfer(cte, body, blen);
        char path[1100];
        if (dir && dir[0]) snprintf(path, sizeof path, "%s/%s", dir, fname); else snprintf(path, sizeof path, "%s", fname);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(data, 1, strlen(data), f); fclose(f); pms_cprintf("已保存附件: %s\n", path); (*saved)++; }
        else { cerr("无法写入附件: %s\n", path); }
        free(data);
    }
}

/* 解析整封邮件，把附件保存到 dir(缺省当前目录) */
static void save_attachments(const char *msg, size_t len, const char *dir) {
    char hdrs[8192] = ""; const char *body = ""; size_t blen = 0;
    const char *sep = strstr(msg, "\r\n\r\n");
    if (sep) { size_t hl = (size_t)(sep - msg); if (hl >= sizeof hdrs) hl = sizeof hdrs - 1; memcpy(hdrs, msg, hl); hdrs[hl] = 0; body = sep + 4; blen = strlen(body); }
    else { size_t hl = len < sizeof hdrs ? len : sizeof hdrs - 1; memcpy(hdrs, msg, hl); hdrs[hl] = 0; }
    char ctt[160] = "", cte[64] = "", cdd[256] = "";
    header_value(hdrs, "Content-Type", ctt, sizeof ctt);
    header_value(hdrs, "Content-Transfer-Encoding", cte, sizeof cte);
    header_value(hdrs, "Content-Disposition", cdd, sizeof cdd);
    int idx = 0, saved = 0;
    walk_entity(ctt, cte, cdd, body, blen, dir, &idx, &saved);
    if (saved == 0) pms_cprintf("该邮件没有附件。\n");
    else if (dir && dir[0]) pms_cprintf("共保存 %d 个附件到 %s。\n", saved, dir);
    else pms_cprintf("共保存 %d 个附件到当前目录。\n");
}
/* ------------------------------------------------------------------ */
/* SMTP                                                                */
/* ------------------------------------------------------------------ */
static int smtp_resp(Stream *s) {
    int code = -1; char line[2048];
    for (;;) {
        int n = gen_read_line(s, line, sizeof line);
        if (n < 0) return -1;
        if (n < 4) continue;
        int c = atoi(line);
        char sep = line[3];
        if (code < 0) code = c;
        if (sep == ' ') return c;
    }
}

static int smtp_cmd(Stream *s, const char *line) {
    stream_write(s, line, strlen(line));
    stream_write(s, "\r\n", 2);
    return smtp_resp(s);
}

/* 发送命令并读取回复；多行 ESMTP 回复累加到 caps 以探测 STARTTLS 等能力 */
static int smtp_cmd_ex(Stream *s, const char *line, char *caps, size_t cap) {
    stream_write(s, line, strlen(line));
    stream_write(s, "\r\n", 2);
    int code = -1; char ln[2048];
    if (caps) caps[0] = 0;
    for (;;) {
        int n = gen_read_line(s, ln, sizeof ln);
        if (n < 0) return -1;
        if (n < 4) continue;
        int c = atoi(ln);
        if (caps && (size_t)strlen(caps) + (size_t)n + 2 < cap) { strcat(caps, ln); strcat(caps, "\n"); }
        if (code < 0) code = c;
        if (ln[3] == ' ') return c;
    }
}

static int smtp_prepare(Stream *s, TLS *t) {
    char banner[1024];
    if (proto_connect(s, t, g_smtp_host, g_smtp_port, g_smtp_tls, banner, sizeof banner) != 0)
        return -1;
    char caps[4096];
    int code = smtp_cmd_ex(s, "EHLO pms.client", caps, sizeof caps);
    if (code != 250) { code = smtp_cmd_ex(s, "HELO pms.client", caps, sizeof caps); if (code != 250) return -1; }
    if (g_smtp_tls == TLS_STARTTLS) {
        if (strstr(caps, "STARTTLS")) {
            code = smtp_cmd_ex(s, "STARTTLS", NULL, 0);
            if (code != 220) return -1;
            if (!PMS_HAVE_OPENSSL) { cerr("本构建未启用 OpenSSL，无法做 STARTTLS 加密\n"); return -1; }
            if (tls_handshake(t, g_smtp_host) != 0) return -1;
            s->tls = t;
            code = smtp_cmd_ex(s, "EHLO pms.client", caps, sizeof caps);
            if (code != 250) return -1;
        } else {
            cerr("SMTP 服务器不支持 STARTTLS(加密)\n");
            return -1;
        }
    }
    return 0;
}

static void smtp_write_dotstuff(Stream *s, const char *text) {
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        while (len > 0 && p[len-1] == '\r') len--;   /* 去掉行尾 CR，统一转 CRLF */
        if (len && p[0] == '.') stream_write(s, ".", 1);
        if (len) stream_write(s, p, len);
        stream_write(s, "\r\n", 2);
        if (!e) break;
        p = e + 1;
    }
    stream_write(s, ".\r\n", 3);
}

/* 仅做 SMTP 认证；返回 0=成功，否则为服务器应答码 */
static int smtp_auth(Stream *s) {
    char b[2048];
    int code = smtp_cmd(s, "AUTH LOGIN");
    if (code == 334) {
        base64_encode(g_user, strlen(g_user), b, sizeof b);
        code = smtp_cmd(s, b);
        base64_encode(g_pass, strlen(g_pass), b, sizeof b);
        code = smtp_cmd(s, b);
        return code == 235 ? 0 : code;
    }
    size_t ul = strlen(g_user), pl = strlen(g_pass);
    char raw[2048]; size_t o = 0;
    raw[o++] = 0; memcpy(raw + o, g_user, ul); o += ul;
    raw[o++] = 0; memcpy(raw + o, g_pass, pl); o += pl;
    base64_encode(raw, o, b, sizeof b);
    char cmd[2100]; snprintf(cmd, sizeof cmd, "AUTH PLAIN %s", b);
    code = smtp_cmd(s, cmd);
    return code == 235 ? 0 : code;
}

static int smtp_send(Stream *s, TLS *t) {
    int code;
    if ((code = smtp_auth(s)) != 0) die("SMTP 登录(AUTH)失败(%d)", code);

    char cmd[1100];
    snprintf(cmd, sizeof cmd, "MAIL FROM:<%s>", g_from);
    code = smtp_cmd(s, cmd);
    if (code != 250) die("发件人地址(%s)被服务器拒绝(%d)", g_from, code);

    char tobuf[2048]; snprintf(tobuf, sizeof tobuf, "%s", g_to);
    char *save = NULL;
    for (char *tok = strtok_r(tobuf, ",;", &save); tok; tok = strtok_r(NULL, ",;", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *e = tok + strlen(tok);
        while (e > tok && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (strchr(tok, '@') == NULL) {
            pms_cprintf("提示：收件人 <%s> 不是完整的邮箱地址(缺少 @)，投递可能会失败或退回；建议写成如 user@parlz.com\n", tok);
        }
        snprintf(cmd, sizeof cmd, "RCPT TO:<%s>", tok);
        code = smtp_cmd(s, cmd);
        if (code != 250) die("收件人 <%s> 被服务器拒绝(%d)", tok, code);
    }

    code = smtp_cmd(s, "DATA");
    if (code != 354) die("DATA 命令被服务器拒绝(%d)", code);

    char date[64], subj_enc[1024], boundary[40];
    rfc2822_date(date, sizeof date);
    encode_mime_word(g_subj ? g_subj : "", subj_enc, sizeof subj_enc);

    /* 正文 → CRLF */
    Buf bodycrlf; buf_init(&bodycrlf);
    if (g_body) {
        to_crlf(g_body, strlen(g_body), &bodycrlf);
    } else if (g_bodyf) {
        FILE *f = fopen(g_bodyf, "rb");
        if (!f) die("无法打开正文文件: %s", g_bodyf);
        char chunk[4096]; size_t n; Buf src; buf_init(&src);
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buf_append(&src, chunk, n);
        fclose(f);
        to_crlf(src.data, src.len, &bodycrlf);
        buf_free(&src);
    }

    Buf msg; buf_init(&msg);
    if (g_nfiles > 0) {
        snprintf(boundary, sizeof boundary, "=_PMS_%u_%u", (unsigned)time(NULL), (unsigned)(g_nfiles + 1000u));
        char hdrs[5120];
        snprintf(hdrs, sizeof hdrs,
            "From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\n"
            "MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"%s\"\r\n\r\n",
            g_from, g_to, subj_enc, date, boundary);
        buf_puts(&msg, hdrs);
        /* 文本正文部分 */
        buf_puts(&msg, "--"); buf_puts(&msg, boundary);
        buf_puts(&msg, "\r\nContent-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n");
        buf_put_b64of(&msg, bodycrlf.data, bodycrlf.len);   /* 正文 base64，中文不再乱码 */
        buf_puts(&msg, "\r\n");
        /* 附件 */
        for (int i = 0; i < g_nfiles; i++) {
            Buf raw; buf_init(&raw);
            if (read_file(g_files[i], &raw) != 0) die("无法读取附件: %s", g_files[i]);
            size_t clen = raw.len * 4 / 3 + 16;
            char *b64 = (char*)malloc(clen);
            if (!b64) die("内存不足");
            base64_encode(raw.data, raw.len, b64, clen);
            buf_free(&raw);
            const char *name = base_name(g_files[i]);
            char name_enc[512]; encode_mime_word(name, name_enc, sizeof name_enc);
            buf_puts(&msg, "--"); buf_puts(&msg, boundary); buf_puts(&msg, "\r\n");
            char part[600];
            snprintf(part, sizeof part,
                "Content-Type: %s; name=\"%s\"\r\n"
                "Content-Transfer-Encoding: base64\r\n"
                "Content-Disposition: attachment; filename=\"%s\"\r\n\r\n",
                mime_from_path(g_files[i]), name_enc, name_enc);
            buf_puts(&msg, part);
            buf_append_b64(&msg, b64);
            buf_puts(&msg, "\r\n");
            free(b64);
        }
        buf_puts(&msg, "--"); buf_puts(&msg, boundary); buf_puts(&msg, "--\r\n");
    } else {
        char hdrs[4096];
        snprintf(hdrs, sizeof hdrs,
            "From: %s\r\nTo: %s\r\nSubject: %s\r\nDate: %s\r\n"
            "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\n"
            "Content-Transfer-Encoding: base64\r\n\r\n",
            g_from, g_to, subj_enc, date);
        buf_puts(&msg, hdrs);
        buf_put_b64of(&msg, bodycrlf.data, bodycrlf.len);   /* 正文 base64，中文不再乱码 */
    }

    smtp_write_dotstuff(s, msg.data);

    code = smtp_resp(s);
    if (code != 250) die("邮件未被服务器接受(%d)", code);

    save_to_sent(msg.data, msg.len);
    buf_free(&msg);
    buf_free(&bodycrlf);
    smtp_cmd(s, "QUIT");
    (void)t;
    return 0;
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */
static int probe_proto(const char *label, const char *host, int port, TlsPref pref, int wants_starttls) {
    TLS t; tls_init(&t); Stream s;
    char banner[1024];
    if (proto_connect(&s, &t, host, port, pref, banner, sizeof banner) != 0) {
        pms_cprintf("%s %s:%d  连接失败(网络/TLS错误)\n", label, host, port);
        return -1;
    }
    pms_cprintf("%s %s:%d  连接成功: %s\n", label, host, port, banner);

    if (s.tls == NULL && strstr(banner, "STARTTLS")) {
        if (!PMS_HAVE_OPENSSL) {
            pms_cprintf("  STARTTLS 需要 OpenSSL(本构建未启用)\n");
        } else if (host == g_imap_host) {
            if (imap_starttls(&s, &t, host) == 0) { pms_cprintf("  STARTTLS 加密成功\n"); }
            else { pms_cprintf("  STARTTLS 加密失败(服务器不支持)\n"); }
        } else if (wants_starttls) {
            char caps[4096];
            int code = smtp_cmd_ex(&s, "EHLO pms.client", caps, sizeof caps);
            if (code == 250 && strstr(caps, "STARTTLS")) {
                code = smtp_cmd_ex(&s, "STARTTLS", NULL, 0);
                if (code == 220 && tls_handshake(&t, host) == 0) { s.tls = &t; pms_cprintf("  STARTTLS 加密成功\n"); }
                else pms_cprintf("  STARTTLS 加密失败(服务器不支持)\n");
            }
        }
    }
    tls_close(&t);
    return 0;
}

static int cmd_probe(void) {
    int rc = 0;
    if (probe_proto("IMAP", g_imap_host, g_imap_port, g_imap_tls, 1) != 0) rc = 1;
    if (probe_proto("SMTP", g_smtp_host, g_smtp_port, g_smtp_tls, 1) != 0) rc = 1;
    return rc;
}

/* ------------------------------------------------------------------ */
/* 配置/入口                                                           */
/* ------------------------------------------------------------------ */
static void load_env(void) {
    const char *e;
    if ((e = getenv("PMS_USER")) && !g_user) g_user = e;
    if ((e = getenv("PMS_PASS")) && !g_pass) g_pass = e;
    if ((e = getenv("PMS_IMAP_HOST"))) g_imap_host = e;
    if ((e = getenv("PMS_IMAP_PORT"))) g_imap_port = atoi(e);
    if ((e = getenv("PMS_SMTP_HOST"))) g_smtp_host = e;
    if ((e = getenv("PMS_SMTP_PORT"))) g_smtp_port = atoi(e);
}

/* ------------------------------------------------------------------ */
/* 本地凭据存储：登录成功后保存，后续命令自动读取，免再次输入               */
/* ------------------------------------------------------------------ */
static const char *cfg_path(void) {
    static char buf[1024];
    const char *home = getenv("HOME");
    if (home) snprintf(buf, sizeof buf, "%s/.pmsrc", home);
    else snprintf(buf, sizeof buf, ".pmsrc");
    return buf;
}

static int cfg_save(const char *user, const char *pass) {
    FILE *f = fopen(cfg_path(), "w");
    if (!f) return -1;
    fprintf(f, "user=%s\n", user);
    fprintf(f, "pass=%s\n", pass);
    fclose(f);
    return 0;
}

/* 用配置文件补全凭据(仅当未通过 -u/-p 或环境变量提供时) */
static void cfg_load(void) {
    if (g_user && g_pass) return;
    FILE *f = fopen(cfg_path(), "r");
    if (!f) return;
    char line[2048];
    static char ubuf[512], pbuf[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "user=", 5) == 0 && !g_user) { snprintf(ubuf, sizeof ubuf, "%s", line + 5); g_user = ubuf; }
        else if (strncmp(line, "pass=", 5) == 0 && !g_pass) { snprintf(pbuf, sizeof pbuf, "%s", line + 5); g_pass = pbuf; }
    }
    fclose(f);
}

static TlsPref parse_tls(const char *v) {
    if (!v) return TLS_PLAIN;
    if (ci_eq_n(v, "starttls", 8)) return TLS_STARTTLS;
    if (ci_eq_n(v, "implicit", 8) || ci_eq_n(v, "tls", 3) || ci_eq_n(v, "ssl", 3)) return TLS_IMPLICIT;
    return TLS_PLAIN;   /* plain / off / none */
}

static void ensure_credentials(void) {
    static char ubuf[512], pbuf[512];
    if (!g_user) {
        fputs("登录账号(邮箱): ", stdout); fflush(stdout);
        if (!fgets(ubuf, sizeof ubuf, stdin)) die("未输入用户名");
        ubuf[strcspn(ubuf, "\r\n")] = 0;
        g_user = ubuf;
    }
    if (!g_pass) {
        prompt_secret("密码: ", pbuf, sizeof pbuf);
        g_pass = pbuf;
    }
    if (!g_from) g_from = g_user;
}
/* ------------------------------------------------------------------ */
/* 命令行 + main                                                        */
/* ------------------------------------------------------------------ */
static void usage(void) {
    pms_cprintf(
        "pms - Parlz Mail Send/Read CLI (POSIX, 自动适配 明文/STARTTLS/隐式TLS)\n"
        "\n"
        "用法:\n"
        "  pms login                                  登录测试并保存账号(下次无需再输入)\n"
        "  pms list [页码]                              列出收件箱(每页5条,默认第1页,最新在前)\n"
        "  pms read <N> [--html]                       读取第 N 封(可读)；--html 存临时 HTML 供 w3m\n"
        "  pms att <N> [目录]                          下载第 N 封的附件到目录(缺省当前目录)\n"
        "  pms spam [页码]                              列出垃圾箱(Junk, 每页5条,最新在前)\n"
        "  pms drafts [页码]                            列出草稿箱(Drafts, 每页5条,最新在前)\n"
        "  pms emptyjunk                               清空垃圾箱(需输入 yes 确认,不可恢复)\n"
        "  pms emptytrash                              清空回收站(Trash, 需输入 yes 确认)\n"
        "  pms search <关键词>                          搜索主题/正文(结果可直接 read)\n"
        "  pms seen <N> / unseen <N>                   标记第 N 封为已读/未读\n"
        "  pms delete <N>                              永久删除第 N 封(不进垃圾箱)\n"
        "  pms junk <N>                                把第 N 封移到垃圾箱(Junk)\n"
        "  pms move <N> <文件夹>                        把第 N 封移动到指定文件夹\n"
        "  pms send -t <addr> [-s <subject>] [-b <text>|--body-file <f>] [--file <附件>...]  发信(成功后自动存到Sent)\n"
        "  pms probe                                   测试服务器连通性(无需账号)\n"
        "\n"
        "参数:\n"
        "  -u, --user <email>       登录账号(否则交互输入；已登录则自动读取)\n"
        "  -p, --pass <password>     密码(否则交互输入；已登录则自动读取)\n"
        "  -f, --from <addr>         发件人 (缺省=账号)\n"
        "  -t, --to <addr>           收件人, 可用 /[,;]/ 分隔多个\n"
        "  -s, --subject <s>         主题\n"
        "  -b, --body <text>         正文\n"
        "      --body-file <f>       从文件读正文\n"
        "  -F, --file <path>         添加附件(可多次；支持完整路径或 ./ 相对路径)\n"
        "      --imap-tls <plain|starttls|implicit>   IMAP 加密方式(默认 plain)\n"
        "      --smtp-tls <plain|starttls|implicit>   SMTP 加密方式(默认 plain)\n"
        "      --insecure             跳过服务器证书校验(用于自签名证书)\n"
        "  -h, --help                帮助\n"
        "\n"
        "环境变量: PMS_USER / PMS_PASS / PMS_IMAP_HOST / PMS_IMAP_PORT / PMS_SMTP_HOST / PMS_SMTP_PORT\n"
        "说明: 首次运行先用 pms login 验证并保存账号(明文存于 $HOME/.pmsrc)，\n"
        "      之后执行 list/read/send 自动读取该文件保存的账号，无需再次输入。\n"
        "      也可用 -u/-p 或环境变量临时覆盖。\n"
        "      delete / move 为写操作，会先要求输入 yes 确认(no 取消；乱输入仅 3 次机会，超次自动取消)。\n"
        "\n"
        "默认服务器: imap.parlz.com:143 (明文) / smtp.parlz.com:465 (明文 AUTH)\n"
        "实测发现:\n"
        "  · 原给的 imap.parlz.com:993 握手即被服务器重置，不能作为 IMAP TLS 端口。\n"
        "  · 服务器在 143 上宣传 STARTTLS，但执行时回复 * BYE [UNAVAILABLE] TLS initialization failed，\n"
        "    即服务端 TLS 损坏，实际只能走明文；SMTP 465 亦为明文(无 STARTTLS)。\n"
        "  因此默认使用明文；若日后服务器修复 TLS，可用 --imap-tls starttls / --smtp-tls starttls 升级。\n"
        "  · TLS(STARTTLS/隐式) 需要本二进制编译时启用 OpenSSL(PMS_HAVE_OPENSSL)。\n");
}

static void parse_args(int argc, char **argv, const char **cmd_out) {
    const char *cmd = argc >= 2 ? argv[1] : NULL;
    *cmd_out = cmd;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-u") || !strcmp(a, "--user"))              { if (i+1 < argc) g_user = argv[++i]; }
        else if (!strcmp(a, "-p") || !strcmp(a, "--pass"))         { if (i+1 < argc) g_pass = argv[++i]; }
        else if (!strcmp(a, "-f") || !strcmp(a, "--from"))         { if (i+1 < argc) g_from = argv[++i]; }
        else if (!strcmp(a, "-t") || !strcmp(a, "--to"))           { if (i+1 < argc) g_to = argv[++i]; }
        else if (!strcmp(a, "-s") || !strcmp(a, "--subject"))      { if (i+1 < argc) g_subj = argv[++i]; }
        else if (!strcmp(a, "-b") || !strcmp(a, "--body"))         { if (i+1 < argc) g_body = argv[++i]; }
        else if (!strcmp(a, "--body-file"))                        { if (i+1 < argc) g_bodyf = argv[++i]; }
        else if (!strcmp(a, "--imap-tls"))                         { if (i+1 < argc) g_imap_tls = parse_tls(argv[++i]); }
        else if (!strcmp(a, "--smtp-tls"))                         { if (i+1 < argc) g_smtp_tls = parse_tls(argv[++i]); }
        else if (!strcmp(a, "--no-starttls"))                      { g_imap_tls = TLS_PLAIN; g_smtp_tls = TLS_PLAIN; }
        else if (!strcmp(a, "--insecure"))                         { g_insecure = 1; }
        else if (!strcmp(a, "--html"))                             { g_html = 1; }
        else if (!strcmp(a, "-F") || !strcmp(a, "--file"))         { if (i+1 < argc && g_nfiles < 32) g_files[g_nfiles++] = argv[++i]; }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help"))         { usage(); exit(0); }
        else if (!strcmp(cmd, "read") && i == 2)                   { /* 消息号，忽略 */ }
        else if (!strcmp(cmd, "list") && i == 2 && isdigit((unsigned char)a[0])) { g_page = atoi(a); }
    }
}

#if PMS_HAVE_OPENSSL
static void tls_global_init(void) {
    SSL_load_client_CA_file(NULL);
    OPENSSL_init_ssl(0, NULL);
}
#else
static void tls_global_init(void) { (void)0; }
#endif

int main(int argc, char **argv) {
    tls_global_init();
    load_env();

    const char *cmd = NULL;
    parse_args(argc, argv, &cmd);
    cfg_load();   /* 读取已保存的账号，免再次输入 */

    int rc = 0;
    if (!cmd || !strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        usage();
    } else if (!strcmp(cmd, "probe")) {
        rc = cmd_probe();
    } else if (!strcmp(cmd, "list")) {
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        if (imap_prepare(&s, &t) != 0) die("IMAP 连接失败(可尝试加 --insecure 或检查主机/端口)");
        if (imap_login(&s, &t, g_user, g_pass) != 0) die("IMAP 登录失败(账号或密码错误)");
        int exists = 0;
        if (imap_select(&s, &t, &exists, "INBOX") != 0) die("打开收件箱(SELECT INBOX)失败");
        imap_list(&s, &t, exists, g_page);
        tls_close(&t);
    } else if (!strcmp(cmd, "read")) {
        if (argc < 3) die("用法: pms read <N>");
        int n = atoi(argv[2]);
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        if (imap_prepare(&s, &t) != 0) die("IMAP 连接失败(可尝试加 --insecure 或检查主机/端口)");
        if (imap_login(&s, &t, g_user, g_pass) != 0) die("IMAP 登录失败(账号或密码错误)");
        int exists = 0;
        if (imap_select(&s, &t, &exists, "INBOX") != 0) die("打开收件箱(SELECT INBOX)失败");
        rc = imap_read(&s, &t, n, exists);
        tls_close(&t);
    } else if (!strcmp(cmd, "send")) {
        if (!g_to) die("发送邮件需用 -t 指定收件人");
        ensure_credentials();
        if (!g_subj) g_subj = "";
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        if (smtp_prepare(&s, &t) != 0) die("SMTP 连接失败(可尝试加 --insecure 或检查主机/端口)");
        if (smtp_send(&s, &t) != 0) die("SMTP 发送失败");
        pms_cprintf("邮件发送成功\n");
        tls_close(&t);
    } else if (!strcmp(cmd, "login")) {
        ensure_credentials();
        int imap_ok = 0, smtp_ok = 0;
        {
            TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
            if (imap_prepare(&s, &t) == 0 && imap_login(&s, &t, g_user, g_pass) == 0) imap_ok = 1;
            tls_close(&t);
        }
        {
            TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
            if (smtp_prepare(&s, &t) == 0 && smtp_auth(&s) == 0) smtp_ok = 1;
            tls_close(&t);
        }
        pms_cprintf("IMAP 登录: %s\n", imap_ok ? "成功" : "失败");
        pms_cprintf("SMTP 登录: %s\n", smtp_ok ? "成功" : "失败");
        if (imap_ok && smtp_ok) {
            if (cfg_save(g_user, g_pass) == 0)
                pms_cprintf("登录成功！账号信息已保存到：\n  %s\n以后执行 list / read / send 无需再输入。\n", cfg_path());
            else
                pms_cprintf("登录成功，但保存账号信息到本地失败。\n");
            rc = 0;
        } else {
            pms_cprintf("登录失败：请检查账号/密码（可加 -u/-p 指定）。\n");
            rc = 1;
        }
    } else if (!strcmp(cmd, "att")) {
        if (argc < 3) die("用法: pms att <序号> [目录]");
        int n = atoi(argv[2]);
        const char *dir = argc >= 4 ? argv[3] : NULL;
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        int exists = 0; imap_open_inbox(&s, &t, &exists);
        if (n < 1 || n > exists) { cerr("无效的邮件序号 %d(范围 1..%d)\n", n, exists); tls_close(&t); rc = 1; }
        else {
            if (dir && dir[0]) mkdir(dir, 0755);   /* 目录不存在则创建(一层) */
            Buf msg; buf_init(&msg);
            if (imap_fetch_msg(&s, &t, n, &msg) != 0) { cerr("读取第 %d 封失败\n", n); rc = 1; }
            else save_attachments(msg.data, msg.len, dir);
            buf_free(&msg);
            tls_close(&t);
        }
    } else if (!strcmp(cmd, "search")) {
        if (argc < 3) die("用法: pms search <关键词>");
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        int exists = 0; imap_open_inbox(&s, &t, &exists);
        imap_search(&s, &t, argv[2]);
        tls_close(&t);
    } else if (!strcmp(cmd, "spam")) {
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        if (imap_prepare(&s, &t) != 0) die("IMAP 连接失败(可尝试加 --insecure 或检查主机/端口)");
        if (imap_login(&s, &t, g_user, g_pass) != 0) die("IMAP 登录失败(账号或密码错误)");
        int exists = 0;
        if (imap_select(&s, &t, &exists, "Junk") != 0) die("打开垃圾箱(Junk)失败");
        imap_list(&s, &t, exists, g_page);
        tls_close(&t);
    } else if (!strcmp(cmd, "drafts")) {
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        if (imap_prepare(&s, &t) != 0) die("IMAP 连接失败(可尝试加 --insecure 或检查主机/端口)");
        if (imap_login(&s, &t, g_user, g_pass) != 0) die("IMAP 登录失败(账号或密码错误)");
        int exists = 0;
        if (imap_select(&s, &t, &exists, "Drafts") != 0) die("打开草稿箱(Drafts)失败");
        imap_list(&s, &t, exists, g_page);
        tls_close(&t);
    } else if (!strcmp(cmd, "emptyjunk") || !strcmp(cmd, "emptytrash")) {
        const char *label = !strcmp(cmd, "emptyjunk") ? "垃圾箱" : "回收站";
        const char *mbox  = !strcmp(cmd, "emptyjunk") ? "Junk"   : "Trash";
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        if (imap_prepare(&s, &t) != 0) die("IMAP 连接失败(可尝试加 --insecure 或检查主机/端口)");
        if (imap_login(&s, &t, g_user, g_pass) != 0) die("IMAP 登录失败(账号或密码错误)");
        int exists = 0;
        if (imap_select(&s, &t, &exists, mbox) != 0) die("打开%s(%s)失败", label, mbox);
        if (exists <= 0) {
            pms_cprintf("%s本来就是空的，无需清理。\n", label);
            tls_close(&t);
        } else {
            char prom[192];
            snprintf(prom, sizeof prom, "确认清空%s？其中 %d 封邮件将被永久删除！", label, exists);
            if (!confirm(prom, 3)) {
                pms_cprintf("已取消清理。\n");
                tls_close(&t);
                rc = 2;
            } else {
                imap_run(&s, "STORE 1:* +FLAGS \\Deleted");
                imap_run(&s, "EXPUNGE");
                pms_cprintf("已清空%s，共删除 %d 封邮件。\n", label, exists);
                tls_close(&t);
            }
        }
    } else if (!strcmp(cmd, "seen") || !strcmp(cmd, "unseen") || !strcmp(cmd, "delete")) {
        if (argc < 3) die("用法: pms %s <序号>", cmd);
        int n = atoi(argv[2]);
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        int exists = 0; imap_open_inbox(&s, &t, &exists);
        if (n < 1 || n > exists) { cerr("无效的邮件序号 %d(范围 1..%d)\n", n, exists); tls_close(&t); rc = 1; }
        else if (!strcmp(cmd, "seen")) {
            imap_run(&s, "STORE %d +FLAGS \\Seen", n);
            pms_cprintf("第 %d 封已标记为已读。\n", n);
        } else if (!strcmp(cmd, "unseen")) {
            imap_run(&s, "STORE %d -FLAGS \\Seen", n);
            pms_cprintf("第 %d 封已标记为未读。\n", n);
        } else { /* delete */
            char prom[128]; snprintf(prom, sizeof prom, "确认删除收件箱第 %d 封邮件？", n);
            if (confirm(prom, 3)) {
                imap_run(&s, "STORE %d +FLAGS \\Deleted", n);
                imap_run(&s, "EXPUNGE");
                pms_cprintf("第 %d 封已删除。\n", n);
            } else {
                pms_cprintf("已取消删除。\n");
                rc = 2;
            }
        }
        tls_close(&t);
    } else if (!strcmp(cmd, "move") || !strcmp(cmd, "junk")) {
        /* pms junk <N> 等价于 pms move <N> Junk */
        int is_junk = !strcmp(cmd, "junk");
        if (is_junk && argc < 3) die("用法: pms junk <序号>");
        if (!is_junk && argc < 4) die("用法: pms move <序号> <文件夹>");
        const char *target = is_junk ? "Junk" : argv[3];
        int n = atoi(argv[2]);
        ensure_credentials();
        TLS t; tls_init(&t); Stream s; stream_init(&s, -1, &t);
        int exists = 0; imap_open_inbox(&s, &t, &exists);
        if (n < 1 || n > exists) { cerr("无效的邮件序号 %d(范围 1..%d)\n", n, exists); tls_close(&t); rc = 1; }
        else {
            char prom[192];
            snprintf(prom, sizeof prom,
                     is_junk ? "确认把收件箱第 %d 封移到垃圾箱？" : "确认把收件箱第 %d 封移动到 “%s”？",
                     n, target);
            if (confirm(prom, 3)) {
                if (imap_run(&s, "COPY %d \"%s\"", n, target) != 0) {
                    cerr("移动失败：目标文件夹不存在(%s)？\n", target);
                    rc = 1;
                } else {
                    imap_run(&s, "STORE %d +FLAGS \\Deleted", n);
                    imap_run(&s, "EXPUNGE");
                    if (is_junk) pms_cprintf("第 %d 封已移到垃圾箱(Junk)。\n", n);
                    else         pms_cprintf("第 %d 封已移动到 %s。\n", n, target);
                }
            } else {
                pms_cprintf("已取消移动。\n");
                rc = 2;
            }
        }
        tls_close(&t);
    } else {
        cerr("未知命令: %s\n", cmd);
        usage();
        rc = 1;
    }

    return rc;
}
