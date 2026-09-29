


/* util.c - string buffer, base64, url, md5 (RFC 1321), gzip, casestrs,
 *           plus a bundled zero-dependency regex engine. */
#include "pweb.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void str_init(str *s, char *fixed, size_t cap)
{
    s->buf = fixed; s->len = 0; s->cap = cap; s->owned = 0;
}
void str_init_m(str *s)
{
    s->cap = 4096;
    s->buf = (char*)malloc(s->cap);
    s->len = 0; s->owned = 1;
    if (!s->buf) { exit(1); }
}
void str_free(str *s)
{
    if (s->owned) { free(s->buf); s->buf = NULL; s->cap = 0; }
}
int str_append_raw(str *s, const void *p, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        if (!s->owned && s->buf) { return -1; }
        size_t nc = s->cap ? s->cap : 4096;
        while (nc < s->len + n + 1) { nc *= 2; }
        char *nb = (char*)realloc(s->buf, nc);
        if (!nb) { return -1; }
        s->buf = nb; s->cap = nc; s->owned = 1;
    }
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = 0;
    return 0;
}
int str_append(str *s, const char *v)
{
    return str_append_raw(s, v, strlen(v));
}
int str_appendf(str *s, const char *fmt, ...)
{
    va_list ap;
    char tmp[4096];
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) { return -1; }
    if ((size_t)n < sizeof(tmp)) { return str_append_raw(s, tmp, (size_t)n); }
    char *big = (char*)malloc((size_t)n + 1);
    if (!big) { return -1; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = str_append_raw(s, big, (size_t)n);
    free(big);
    return r;
}

void url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            *w++ = (char)strtol(r + 1, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

void url_encode(str *out, const char *s, int is_path)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            strchr("-._~!*'", c) != NULL ||
            (is_path && c == '/') || (is_path && c == ':')) {
            str_append_raw(out, &c, 1);
        } else {
            char e[4];
            snprintf(e, sizeof(e), "%%%02X", c);
            str_append(out, e);
        }
    }
}

char *b64_decode(const char *src, size_t n, size_t *outlen)
{
    static const char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int rev[256];
    memset(rev, -1, sizeof(rev));
    for (int i = 0; i < 64; i++) { rev[(unsigned char)T[i]] = i; }
    if (n == 0) {
        char *out = (char*)malloc(1);
        if (out) { out[0] = 0; }
        if (outlen) { *outlen = 0; }
        return out;
    }
    size_t olen = n / 4 * 3 + 3;
    char *out = (char*)malloc(olen + 1);
    if (!out) { return NULL; }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (c == '=') { break; }
        if (rev[(unsigned char)c] < 0) { free(out); return NULL; }
        int v = rev[(unsigned char)c];
        if (i % 4 == 0) { out[o++] = (char)(v << 2); }
        else if (i % 4 == 1) {
            out[o-1] = (char)((out[o-1] & 0xfc) | (v >> 4));
            out[o++] = (char)(v << 4);
        }
        else if (i % 4 == 2) {
            out[o-1] = (char)((out[o-1] & 0xf0) | (v >> 2));
            out[o++] = (char)((v & 0x3) << 6);
        }
        else {
            out[o-1] = (char)((out[o-1] & 0xc0) | v);
        }
    }
    if (src[n-1] == '=') { o--; }
    if (n > 1 && src[n-2] == '=') { o--; }
    if (outlen) { *outlen = o; }
    out[o] = 0;
    return out;
}

/* portable case-insensitive strstr */
const char *casestrs(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0) { return hay; }
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nl) == 0) { return hay; }
    }
    return NULL;
}

/* ---------- MD5 (RFC 1321) ---------- */
static void md5_compress(unsigned int state[4], const unsigned char block[64])
{
    static const unsigned int K[64] = {
        0xd76aa6e8,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
        0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x9b564231,0xcd7e6c9f,0x92cd49fa,0x5cb0c98c,0x99b34d2f,0xc2774550,
        0x2dd82763,0x5d469d22,0xa5e79384,0x944b1e26,0xef8bb088,0x879d9b2a,
        0xf4f57069,0xcbb0a229,0x2e66b1d6,0xcb8925e5,0x440f3b94,0xc7c1d8e0,
        0x8b5072e6,0x6f9a294a,0x7e64e228,0x861c62f8,0x862790f4,0xb4c76c3c,
        0xd348694a,0x626e33e8,0xd1bf5871,0x567a02e1,0xc2e2db12,0x71e52887,
        0x44c149ac,0x9224261e,0x63705e2f,0x0b3222db,0x42f5e989,0x86086d38,
        0xa5549846,0x5db73013,0x74563680,0xef0a8e5a,0x9e6022a1,0x85e56cb0,
        0xd84e0f50,0xe851e274,0xec3e2f88,0x15f38d20,0x1a1445d5,0x3c67f95a,
        0xb0c65572,0x4c8f8164,0xb213244f,0xcc1a4888,0x6c5a1a42,0xd6c62046,
        0x9b8f19ac,0xc199e1f5,0xd2ca81f6,0x2ac516b2,0xd1f8f7d5,0x31d32120,
        0x45d049d8,0x475f64a2,0xb945228f,0xc4e1d0a1
    };
    static const int S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    unsigned int M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = (unsigned int)block[i*4] | ((unsigned int)block[i*4+1]<<8) |
               ((unsigned int)block[i*4+2]<<16) | ((unsigned int)block[i*4+3]<<24);
    }
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        unsigned int x, f;
        if (i < 16) {
            f = (b & c) | (~b & d);
            x = f + a + K[i] + M[i];
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            x = f + a + K[i] + M[(5*i+1) & 15];
        } else if (i < 48) {
            f = b ^ c ^ d;
            x = f + a + K[i] + M[(3*i+5) & 15];
        } else {
            f = c ^ (b | ~d);
            x = f + a + K[i] + M[(7*i) & 15];
        }
        x = (x + b) & 0xffffffff;
        x = (x << S[i]) | (x >> (32 - S[i]));
        a = d; d = c; c = b; b = x;
    }
    state[0] = (state[0] + a) & 0xffffffff;
    state[1] = (state[1] + b) & 0xffffffff;
    state[2] = (state[2] + c) & 0xffffffff;
    state[3] = (state[3] + d) & 0xffffffff;
}

int pweb_md5(const void *d, size_t n, unsigned char *out16)
{
    unsigned int state[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const unsigned char *p = d;
    unsigned int bits = (unsigned int)(n * 8);
    while (n >= 64) {
        md5_compress(state, p);
        p += 64; n -= 64;
    }
    unsigned char block[64];
    memset(block, 0, 64);
    memcpy(block, p, n);
    block[n] = 0x80;
    size_t pad = n < 56 ? 56 : 64;
    for (int i = 0; i < 8; i++) {
        block[pad + i] = (unsigned char)((bits >> (i*8)) & 0xff);
    }
    md5_compress(state, block);
    if (n >= 56) {
        unsigned char zero[64];
        memset(zero, 0, 64);
        md5_compress(state, zero);
    }
    for (int i = 0; i < 4; i++) {
        out16[i*4+0] = (unsigned char)(state[i] & 0xff);
        out16[i*4+1] = (unsigned char)((state[i] >> 8) & 0xff);
        out16[i*4+2] = (unsigned char)((state[i] >> 16) & 0xff);
        out16[i*4+3] = (unsigned char)((state[i] >> 24) & 0xff);
    }
    return 0;
}
void md5_hex(const void *data, size_t n, char out[33])
{
    unsigned char outb[16];
    pweb_md5(data, n, outb);
    for (int i = 0; i < 16; i++) { sprintf(out + i*2, "%02x", outb[i]); }
    out[32] = 0;
}


/* ---------- gzip writer (RFC1952, stored deflate blocks + CRC32) ---------- */
static unsigned int crc32_table[256];
static int crc32_init = 0;
static unsigned int crc32_update(unsigned int c, const unsigned char *p,
                                  size_t n)
{
    if (!crc32_init) {
        crc32_init = 1;
        for (unsigned i = 0; i < 256; i++) {
            unsigned r = i;
            for (int k = 0; k < 8; k++) {
                r = (r & 1) ? (r >> 1) ^ 0xedb88320u : (r >> 1);
            }
            crc32_table[i] = r;
        }
    }
    for (size_t i = 0; i < n; i++) {
        c = crc32_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    return c;
}

char *pweb_gzip(const void *data, size_t n, size_t *outlen)
{
    size_t block_chunk = 65535;
    int nblocks = (int)((n + block_chunk - 1) / block_chunk) + 1;
    size_t cap = 10 + n + (size_t)nblocks * 5 + 8;
    char *out = (char*)malloc(cap);
    if (!out) { return NULL; }
    size_t o = 0;
    out[o++] = 0x1f; out[o++] = 0x8b; out[o++] = 0x08;
    out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
    out[o++] = 0; out[o++] = 3;
    unsigned int crc = 0xffffffff;
    for (size_t off = 0; off < n; off += block_chunk) {
        size_t chunk = n - off;
        int last = (off + chunk >= n) ? 1 : 0;
        unsigned int len = chunk;
        if (len > block_chunk) { len = block_chunk; }
        out[o++] = (char)(last ? 1 : 0);
        out[o++] = (char)(len & 0xff);
        out[o++] = (char)((len >> 8) & 0xff);
        out[o++] = (char)(~len & 0xff);
        out[o++] = (char)((~len >> 8) & 0xff);
        memcpy(out + o, (const unsigned char*)data + off, len);
        o += len;
        crc = crc32_update(crc, (const unsigned char*)data + off, len);
    }
    crc ^= 0xffffffff;
    out[o++] = (char)(crc & 0xff);
    out[o++] = (char)((crc >> 8) & 0xff);
    out[o++] = (char)((crc >> 16) & 0xff);
    out[o++] = (char)((crc >> 24) & 0xff);
    unsigned int isize = (unsigned int)(n & 0xffffffff);
    out[o++] = (char)(isize & 0xff);
    out[o++] = (char)((isize >> 8) & 0xff);
    out[o++] = (char)((isize >> 16) & 0xff);
    out[o++] = (char)((isize >> 24) & 0xff);
    if (outlen) { *outlen = o; }
    return out;
}
/* ===================================================================
 * Bundled POSIX-subset regex engine: capture groups + $n substitution.
 * Zero dependencies (<regex.h> unavailable on the MinGW toolchain).
 * Supports: literals, ., [] classes, ( ) capture (max 9),
 * quantifiers * + ? {n,m}, \ escapes, ^ $ anchors.
 * No alternation |, backrefs \1, or lookahead (not needed for
 * nginx-style rewrite patterns).
 * =================================================================== */
#define PWEB_RE_MAXCAPS 9

typedef struct {
    int kind;   /* 0 literal, 1 dot, 2 class, 3 grp-open, 4 grp-close,
                  5 ^-anchor, 6 $-anchor */
    int val;
    unsigned char cls[128];
    int qtype;  /* 0 none, 1 *, 2 +, 3 ?, 4 {n,m} */
    int qn, qm;
} retok;

static retok rg[256];
static int rn = 0;

static void re_parse_class(const char **pp, retok *t)
{
    const char *p = *pp;
    unsigned char present[128];
    memset(present, 0, 128);
    t->kind = 2;
    if (*p == '[') { p++; }
    int neg = 0;
    if (*p == '^') { neg = 1; p++; }
    while (*p && *p != ']') {
        char lo = *p++;
        char hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') { p++; hi = *p++; }
        for (int v = lo; v <= hi; v++) { present[(unsigned char)v] = 1; }
    }
    if (*p == ']') { p++; }
    for (int i = 0; i < 128; i++) {
        t->cls[i] = neg ? (!present[i]) : present[i];
    }
    *pp = p;
}

/* Parse pat into rg[]; 0 ok, -1 malformed. */
int pweb_re_compile(const char *pat)
{
    rn = 0;
    memset(rg, 0, sizeof(rg));
    const char *p = pat;
    while (*p && rn < 256) {
        retok t;
        memset(&t, 0, sizeof(t));
        if (*p == '(') {
            t.kind = 3; p++;
        } else if (*p == ')') {
            t.kind = 4; p++;
        } else if (*p == '^') {
            t.kind = 5; p++;
        } else if (*p == '$') {
            t.kind = 6; p++;
        } else if (*p == '[') {
            re_parse_class(&p, &t);
        } else if (*p == '.') {
            t.kind = 1; p++;
        } else if (*p == '\\') {
            p++;
            if (!*p) { return -1; }
            t.kind = 0; t.val = (int)*p; p++;
        } else {
            t.kind = 0; t.val = (int)*p; p++;
        }
        /* quantifier attaches to the atom just emitted */
        if (t.kind >= 0 && t.kind <= 2) {
            if (*p == '*') { t.qtype = 1; p++; }
            else if (*p == '+') { t.qtype = 2; p++; }
            else if (*p == '?') { t.qtype = 3; p++; }
            else if (*p == '{') {
                p++;
                int a = 0, ha = 0;
                while (isdigit((unsigned char)*p)) { a = a * 10 + (*p - '0'); p++; ha = 1; }
                t.qn = ha ? a : 0;
                if (*p == ',') {
                    p++;
                    int b = 0, hb = 0;
                    while (isdigit((unsigned char)*p)) { b = b * 10 + (*p - '0'); p++; hb = 1; }
                    t.qm = hb ? b : 255;
                    t.qtype = 4;
                } else if (*p == '}') {
                    t.qm = a; t.qtype = 4; p++;
                } else {
                    return -1;
                }
            }
        }
        rg[rn++] = t;
    }
    return 0;
}

/* Does atom t (kind 0/1/2) match the single char at pos? */
static int re_one(const retok *t, const char *pos)
{
    if (*pos == 0) { return 0; }
    if (t->kind == 0) { return *pos == (char)t->val; }
    if (t->kind == 1) { return 1; }
    if (t->kind == 2) { return t->cls[(unsigned char)*pos] != 0; }
    return 0;
}

/* Forward decl: group-interior matcher (used by re_match). */
static int re_grp_interior(int lo, int hi, const char *pos, int depth,
                           int *capst, int *caplen, int *ng,
                           const char *str_start, const char *str_end,
                           const char **outp);

/* Recursive backtracking matcher over rg[0..rn).
 * Top-level entry. capst/caplen store absolute offsets of capture groups. */
static int re_match(int ti, const char *pos, int depth,
                    int *capst, int *caplen, int *ng,
                    const char *str_start, const char *str_end)
{
    if (depth > 4096) { return 0; }
    if (ti == rn) { return 1; }
    const retok *t = &rg[ti];

    if (t->kind == 4) {
        return re_match(ti + 1, pos, depth, capst, caplen, ng, str_start, str_end);
    }
    if (t->kind == 5) {
        if (pos != str_start) { return 0; }
        return re_match(ti + 1, pos, depth, capst, caplen, ng, str_start, str_end);
    }
    if (t->kind == 6) {
        if (pos != str_end) { return 0; }
        return re_match(ti + 1, pos, depth, capst, caplen, ng, str_start, str_end);
    }

    if (t->kind == 3) {
        /* group open: find matching close, match interior, then backtrack
         * the group's start position until the rest of the pattern matches. */
        int g = *ng;
        int hi = ti + 1;
        int bal = 1;
        while (hi < rn && bal > 0) {
            if (rg[hi].kind == 3) { bal++; hi++; }
            else if (rg[hi].kind == 4) { bal--; if (bal == 0) { break; } hi++; }
            else { hi++; }
        }
        if (hi >= rn || rg[hi].kind != 4) { return 0; }
        const char *gstart = pos;
        for (;;) {
            if (gstart > str_end) { break; }
            if (g < PWEB_RE_MAXCAPS) { capst[g] = (int)(gstart - str_start); }
            const char *gp = gstart;
            int ng_inner = *ng;
            int ok = re_grp_interior(ti + 1, hi, gp, depth + 1,
                                     capst, caplen, &ng_inner,
                                     str_start, str_end, &gp);
            if (ok) {
                if (g < PWEB_RE_MAXCAPS) { caplen[g] = (int)(gp - gstart); }
                *ng = ng_inner;
                /* continue matching the rest of the pattern from gp,
                 * skipping the close token at hi */
                return re_match(hi + 1, gp, depth + 1, capst, caplen, ng,
                                str_start, str_end);
            }
            gstart++;
        }
        if (g < PWEB_RE_MAXCAPS) { capst[g] = -1; caplen[g] = 0; }
        return 0;
    }

    /* single-char atom with quantifier (kind 0/1/2) */
    if (t->qtype == 0) {
        if (!re_one(t, pos)) { return 0; }
        return re_match(ti + 1, pos + 1, depth, capst, caplen, ng, str_start, str_end);
    }
    if (t->qtype == 3) {
        /* ? : zero or one (greedy) */
        if (re_one(t, pos) &&
            re_match(ti + 1, pos + 1, depth + 1, capst, caplen, ng, str_start, str_end)) {
            return 1;
        }
        return re_match(ti + 1, pos, depth, capst, caplen, ng, str_start, str_end);
    }
    if (t->qtype == 1 || t->qtype == 2) {
        /* * or + : greedy with backtracking */
        int lo = (t->qtype == 2) ? 1 : 0;
        int cnt = 0;
        const char *p2 = pos;
        while (re_one(t, p2)) { p2++; cnt++; }
        for (int c = cnt; c >= lo; c--) {
            const char *try2 = pos;
            for (int k = 0; k < c; k++) { try2++; }
            if (re_match(ti + 1, try2, depth + 1, capst, caplen, ng,
                         str_start, str_end)) {
                return 1;
            }
        }
        return 0;
    }
    if (t->qtype == 4) {
        /* {n,m} greedy with backtracking */
        int hi2 = t->qm;
        int matched = 0;
        const char *q = pos;
        while (re_one(t, q)) { q++; matched++; }
        int hi = (hi2 < matched) ? hi2 : matched;
        for (int c = hi; c >= t->qn; c--) {
            const char *try2 = pos;
            for (int k = 0; k < c; k++) { try2++; }
            if (re_match(ti + 1, try2, depth + 1, capst, caplen, ng,
                         str_start, str_end)) {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

/* Match a group interior [lo,hi) where hi is the matching close-token index.
 * Greedy single-char atoms inside a group take the maximum run (sufficient
 * for nginx rewrite patterns, where group bodies are literal/dot/class).
 * On success, *outp points just past the matched content. */
static int re_grp_interior(int lo, int hi, const char *pos, int depth,
                           int *capst, int *caplen, int *ng,
                           const char *str_start, const char *str_end,
                           const char **outp)
{
    /* Match tokens [lo, hi) — hi is the index of the matching group-close,
     * which is NOT consumed here (the close token consumes no input). */
    if (depth > 4096) { return 0; }
    const char *cur = pos;
    for (int i = lo; i < hi; i++) {
        const retok *t = &rg[i];
        if (t->kind == 4) { continue; }
        if (t->kind == 3) {
            /* nested group: find its close, match its interior, backtrack start */
            int g = *ng;
            int chi = i + 1;
            int bal = 1;
            while (chi < rn && bal > 0) {
                if (rg[chi].kind == 3) { bal++; chi++; }
                else if (rg[chi].kind == 4) { bal--; if (bal == 0) { break; } chi++; }
                else { chi++; }
            }
            if (chi >= rn || rg[chi].kind != 4) { return 0; }
            const char *gstart = cur;
            int ok = 0;
            for (;;) {
                if (gstart > str_end) { break; }
                if (g < PWEB_RE_MAXCAPS) { capst[g] = (int)(gstart - str_start); }
                const char *gp = gstart;
                int ng_i = *ng;
                if (re_grp_interior(i + 1, chi, gp, depth + 1,
                                    capst, caplen, &ng_i,
                                    str_start, str_end, &gp)) {
                    if (g < PWEB_RE_MAXCAPS) { caplen[g] = (int)(gp - gstart); }
                    *ng = ng_i;
                    cur = gp;
                    i = chi;
                    ok = 1;
                    break;
                }
                gstart++;
            }
            if (!ok) {
                if (g < PWEB_RE_MAXCAPS) { capst[g] = -1; caplen[g] = 0; }
                return 0;
            }
            continue;
        }
        if (t->kind == 5) { if (cur != str_start) { return 0; } continue; }
        if (t->kind == 6) { if (cur != str_end) { return 0; } continue; }
        /* single-char atom with quantifier */
        if (t->qtype == 0) {
            if (!re_one(t, cur)) { return 0; }
            cur++;
        } else if (t->qtype == 3) {
            if (re_one(t, cur)) { cur++; }
        } else {
            int lo2 = (t->qtype == 4) ? t->qn : (t->qtype == 2 ? 1 : 0);
            int hi2 = (t->qtype == 4) ? t->qm : 999;
            int cnt = 0;
            const char *p2 = cur;
            while (cnt < hi2 && re_one(t, p2)) { p2++; cnt++; }
            if (cnt < lo2) { return 0; }
            /* greedy: take the max run; no cross-token backtracking needed
             * for nginx rewrite group bodies (literal/dot/class). */
            cur = p2;
        }
    }
    *outp = cur;
    return 1;
}

/* pweb_re_match: compile pat, match s (search semantics: if the pattern has
 * no ^ anchor, try matching at every start offset). caps[i] = group i. */
int pweb_re_match(const char *pat, const char *s, char caps[][512], int *ncaps)
{
    if (pweb_re_compile(pat) != 0) { return 0; }
    int capst[PWEB_RE_MAXCAPS + 1];
    int caplen[PWEB_RE_MAXCAPS + 1];
    int ng = 0;
    memset(capst, -1, sizeof(capst));
    memset(caplen, 0, sizeof(caplen));
    int ngcnt = 0;
    for (int i = 0; i < rn; i++) { if (rg[i].kind == 3) { ngcnt++; } }
    if (ncaps) { *ncaps = ngcnt; }
    const char *str_start = s;
    const char *str_end = s + strlen(s);
    int has_anchor = 0;
    for (int i = 0; i < rn; i++) { if (rg[i].kind == 5) { has_anchor = 1; break; } }
    int ok = 0;
    const char *try = s;
    for (;;) {
        int ng2 = 0;
        int capst2[PWEB_RE_MAXCAPS + 1];
        int caplen2[PWEB_RE_MAXCAPS + 1];
        memcpy(capst2, capst, sizeof(capst));
        memcpy(caplen2, caplen, sizeof(caplen));
        if (re_match(0, try, 0, capst2, caplen2, &ng2, s, str_end)) {
            ok = 1;
            memcpy(capst, capst2, sizeof(capst));
            memcpy(caplen, caplen2, sizeof(caplen));
            break;
        }
        if (has_anchor || try >= str_end) { break; }
        try++;
    }
    if (caps && ok) {
        for (int g = 0; g < PWEB_RE_MAXCAPS; g++) {
            int off = capst[g];
            int L = caplen[g];
            if (off >= 0 && L >= 0 && off + L <= (int)strlen(s)) {
                if (L > 511) { L = 511; }
                memcpy(caps[g], s + off, (size_t)L);
                caps[g][L] = 0;
            } else {
                caps[g][0] = 0;
            }
        }
    }
    return ok;
}

/* pweb_re_sub: expand $1..$9 in repl using the match of pat on s.
 * No match => out = s, returns 0. Match => out = substituted, returns 1. */
int pweb_re_sub(const char *pat, const char *s, const char *repl,
                char *out, size_t n)
{
    char caps[PWEB_RE_MAXCAPS][512];
    int nc = 0;
    if (!pweb_re_match(pat, s, caps, &nc)) {
        if (n > 0) { snprintf(out, n, "%s", s); }
        return 0;
    }
    size_t o = 0;
    for (const char *r = repl; *r && o < n - 1; r++) {
        if (*r == '$' && r[1] >= '1' && r[1] <= '9') {
            int gi = r[1] - '1';
            const char *cg = (gi < nc) ? caps[gi] : "";
            for (; *cg && o < n - 1; cg++) { out[o++] = *cg; }
            r++;
        } else if (*r == '$' && r[1] == '$') {
            out[o++] = '$'; r++;
        } else {
            out[o++] = *r;
        }
    }
    out[o] = 0;
    return 1;
}

