/* w3m.c - w3m 风格文本浏览器(自研, 无外部终端库依赖, 中文输出)。
 *
 * 三层架构:
 *   1. HTML 词法 -> DOM 树(标签/属性/文本/实体 &lt;&gt;&amp;&quot;&nbsp; + &#NN;)
 *   2. CSS: <style> 块 + 内联 style 属性 -> 规则列表; 遍历 DOM 按选择器
 *      匹配, 合并成每节点"计算样式"(优先级: 内联 > #id > .class > 标签/*)
 *   3. 终端渲染: 块级换行/行内连续, 列表 -/1. 前缀, hr 分隔线, pre 保空白,
 *      img 占位, 按终端宽度折行, ANSI(24bit 颜色/粗体/斜体), display:none
 *      跳过子树, 底部链接表。
 *
 * 支持标签(最小集): html/head/body/title, h1~h6, p, br, hr, div, span,
 *   a[href], ul/ol/li, pre, b/strong, i/em, img, style, script。
 * 支持 CSS 属性: display:none, color, background-color,
 *   font-weight:bold, font-size:Npx。
 * 选择器: * / 标签 / .class / #id / 标签.class(复合)。
 *
 * 用法: w3m [URL|文件]
 *   本地路径 / file:// 直接读; http(s):// 复用 http_client(自动跟 3xx)。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include "http_client.h"

#define DOM_CAP      8192
#define PAGE_BUF_CAP (256 * 1024)
#define CSS_RULE_CAP 128
#define LINK_CAP     2048
#define COMP_CAP     16384
#define TXT_CAP      8192

/* ================= 计算样式 ================= */
struct cstyle {
    int fg[3], bg[3];   /* 24bit; 全 -1 = 未设 */
    int bold, italic;
    int fontsize;      /* 16 = 默认; 终端无真缩放, 仅 >16 时行尾标注 */
    int disp_none;
};

static struct cstyle empty_style(void)
{
    struct cstyle s;
    memset(&s, 0, sizeof s);
    s.fg[0] = s.fg[1] = s.fg[2] = -1;
    s.bg[0] = s.bg[1] = s.bg[2] = -1;
    s.fontsize = 16;
    return s;
}

/* ================= CSS 规则表 ================= */
struct css_rule {
    char selector[64];
    struct cstyle st;
};
static struct css_rule g_rules[CSS_RULE_CAP];
static int g_nrules = 0;

/* ================= DOM 树 ================= */
struct node {
    char tag[32];       /* 小写标签名; 文本节点为空串 */
    int  is_text;
    char *text;         /* 文本内容(已解码实体), 仅 is_text */
    int  first, next, last;   /* 子/兄弟索引, -1 = 无 */
    int  parent;
    char cls[64];
    char id[64];
    char href[512];
    char style[256];
    char alt[128];
    int  comp;          /* 计算样式槽, -1 = 无 */
};

struct page {
    struct node nodes[DOM_CAP];
    int nnodes;
    int root;
    char *title;
    struct cstyle comp[COMP_CAP];
    int ncomp;
    struct {
        char text[256];
        char target[512];
    } links[LINK_CAP];
    int nlinks;
};

static struct page *page_new(void)
{
    struct page *pg = calloc(1, sizeof *pg);
    if (!pg)
        return NULL;
    for (int i = 0; i < DOM_CAP; i++) {
        pg->nodes[i].first = pg->nodes[i].next =
            pg->nodes[i].last = -1;
        pg->nodes[i].parent = -1;
        pg->nodes[i].comp = -1;
    }
    return pg;
}

static void page_free(struct page *pg)
{
    if (!pg)
        return;
    for (int i = 0; i < pg->nnodes; i++)
        free(pg->nodes[i].text);
    free(pg->title);
    free(pg);
}

static int alloc_comp(struct page *pg, const struct cstyle *s)
{
    if (pg->ncomp >= COMP_CAP)
        return -1;
    pg->comp[pg->ncomp] = *s;
    return pg->ncomp++;
}

static int node_new(struct page *pg, const char *tag, int is_text)
{
    if (pg->nnodes >= DOM_CAP)
        return -1;
    int i = pg->nnodes++;
    struct node *n = &pg->nodes[i];
    memset(n, 0, sizeof *n);
    n->first = n->next = n->last = n->comp = -1;
    n->parent = -1;
    if (is_text)
        n->is_text = 1;
    else {
        size_t tl = strnlen(tag, 31);
        memcpy(n->tag, tag, tl);
        for (size_t k = 0; k < tl; k++)
            n->tag[k] = tolower((unsigned char)n->tag[k]);
    }
    return i;
}

static void node_add_child(struct page *pg, int parent, int child)
{
    struct node *p = &pg->nodes[parent];
    struct node *c = &pg->nodes[child];
    c->parent = parent;
    c->next = -1;
    if (p->first == -1)
        p->first = p->last = child;
    else
        pg->nodes[p->last].next = child;
    p->last = child;
}

static int copy_str(char *dst, const char *src, size_t cap)
{
    if (!src)
        return 0;
    size_t l = strnlen(src, cap - 1);
    memcpy(dst, src, l);
    dst[l] = 0;
    return (int)l;
}

/* ================= HTML 实体 ================= */
static size_t decode_entities(char *dst, const char *src, size_t len)
{
    size_t o = 0;
    while (len) {
        char ch = *src;
        if (ch != '&') {
            dst[o++] = *src++;
            len--;
            continue;
        }
        /* 命名实体(按最长匹配, 长度不足则原样保留) */
        if (!strncmp(src, "&lt;", 4) && len >= 4)      { dst[o++] = '<';  src += 4; len -= 4; continue; }
        if (!strncmp(src, "&gt;", 4) && len >= 4)      { dst[o++] = '>';  src += 4; len -= 4; continue; }
        if (!strncmp(src, "&amp;", 5) && len >= 5)     { dst[o++] = '&';  src += 5; len -= 5; continue; }
        if (!strncmp(src, "&quot;", 6) && len >= 6)    { dst[o++] = '"';  src += 6; len -= 6; continue; }
        if (!strncmp(src, "&nbsp;", 6) && len >= 6)    { dst[o++] = ' ';  src += 6; len -= 6; continue; }
        /* 数字实体 &#NN; / &#xHH; */
        if (src[1] == '#' && len >= 3) {
            const char *e = src + 2;
            int hex = 0, nd = 0;
            if (*e == 'x' || *e == 'X') { hex = 1; e++; }
            int val = 0;
            while (*e && *e != ';') {
                char c = *e++;
                int d = isdigit((unsigned char)c) ? c - '0'
                        : hex ? tolower((unsigned char)c) - 'a' + 10 : -1;
                if (d < 0 || nd >= 6)
                    break;
                val = val * (hex ? 16 : 10) + d;
                nd++;
            }
            if (*e == ';' && nd > 0 && val > 0 && val <= 0x10ffff) {
                unsigned v = (unsigned)val;
                int wb = 1;
                if (v >= 0x10000) {
                    dst[o++] = 0xf0 | (v >> 18);
                    dst[o++] = 0x80 | ((v >> 12) & 0x3f);
                    dst[o++] = 0x80 | ((v >> 6) & 0x3f);
                    dst[o++] = 0x80 | (v & 0x3f);
                    wb = 4;
                } else if (v >= 0x800) {
                    dst[o++] = 0xe0 | (v >> 12);
                    dst[o++] = 0x80 | ((v >> 6) & 0x3f);
                    dst[o++] = 0x80 | (v & 0x3f);
                    wb = 3;
                } else if (v >= 0x80) {
                    dst[o++] = 0xc0 | (v >> 6);
                    dst[o++] = 0x80 | (v & 0x3f);
                    wb = 2;
                } else {
                    dst[o++] = (char)v;
                }
                /* e 停在 ';', 需越过整个实体(从 src 到 e+1) */
                len -= (size_t)(e - src) + 1;
                src = e + 1;
                continue;
            }
        }
        /* 未知实体: 原样保留 & */
        dst[o++] = '&';
        src++;
        len--;
    }
    dst[o] = 0;
    return o;
}

/* 取标签串中 name 属性值(支持 name="v" / name='v' / name=v) */
static int tag_attr(const char *tag, const char *name, char *val, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = tag;
    while ((p = strpbrk(p, " \t\n")) != NULL) {
        p++;  /* 跳过空白进入属性名 */
        if (!*p || *p == '>')
            return 0;
        const char *hit = strstr(p, name);
        if (hit && (hit == p || isspace((unsigned char)hit[-1]))) {
            const char *a = hit + nl;
            while (*a == ' ' || *a == '\t')
                a++;
            if (*a != '=') {
                p = hit;
                continue;
            }
            a++;
            while (*a == ' ' || *a == '\t')
                a++;
            const char *v;
            size_t vl;
            if (*a == '"' || *a == '\'') {
                char q = *a++;
                v = a;
                while (*a && *a != q)
                    a++;
                vl = (size_t)(a - v);
            } else {
                v = a;
                while (*a && !isspace((unsigned char)*a) && *a != '>')
                    a++;
                vl = (size_t)(a - v);
            }
            vl = vl < cap - 1 ? vl : cap - 1;
            memcpy(val, v, vl);
            val[vl] = 0;
            return 1;
        }
        p = hit ? hit : p + 1;
    }
    return 0;
}

/* 自闭合标签(开标签不压栈, 不入子树) */
static int is_self_closing(const char *t)
{
    return !strcasecmp(t, "br") || !strcasecmp(t, "hr") ||
           !strcasecmp(t, "img") || !strcasecmp(t, "input") ||
           !strcasecmp(t, "meta") || !strcasecmp(t, "link") ||
           !strcasecmp(t, "col");
}

/* 块级标签(渲染换行 + 前后空行由渲染层决定) */

/* ================= CSS 解析 ================= */

/* 颜色名/十六进制 -> 24bit rgb; 成功 0, 失败 -1 */
static int css_color(const char *name, int rgb[3])
{
    if (!name || !*name)
        return -1;
    rgb[0] = rgb[1] = rgb[2] = -1;
    if (name[0] == '#') {
        size_t hl = strnlen(name + 1, 7);
        if (hl < 3)
            return -1;
        unsigned long v = strtoul(name + 1, NULL, 16);
        if (hl == 3)
            v = ((v >> 8) & 0xf) * 17 | ((v >> 4) & 0xf) * 17 | (v & 0xf) * 17;
        rgb[0] = (int)((v >> 16) & 0xff);
        rgb[1] = (int)((v >> 8) & 0xff);
        rgb[2] = (int)(v & 0xff);
        return 0;
    }
    /* 命名色(常用子集) */
    static const struct {
        const char *n;
        int r, g, b;
    } names[] = {
        {"black", 0, 0, 0},       {"white", 255, 255, 255},
        {"red", 255, 0, 0},       {"green", 0, 128, 0},
        {"lime", 0, 255, 0},     {"blue", 0, 0, 255},
        {"navy", 0, 0, 128},     {"yellow", 255, 255, 0},
        {"orange", 255, 165, 0}, {"purple", 128, 0, 128},
        {"violet", 238, 130, 238},{"magenta", 255, 0, 255},
        {"cyan", 0, 255, 255},   {"teal", 0, 128, 128},
        {"gray", 128, 128, 128}, {"grey", 128, 128, 128},
        {"maroon", 128, 0, 0},   {"olive", 128, 128, 0},
        {"silver", 192, 192, 192},{"transparent", -1, -1, -1},
    };
    char lc[32];
    size_t l = strnlen(name, sizeof lc - 1);
    for (size_t i = 0; i < l; i++)
        lc[i] = tolower((unsigned char)name[i]);
    lc[l] = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(lc, names[i].n) == 0) {
            rgb[0] = names[i].r;
            rgb[1] = names[i].g;
            rgb[2] = names[i].b;
            if (rgb[0] == -1)
                return -1; /* transparent */
            return 0;
        }
    /* rgb(r,g,b) 形式 */
    if (!strncasecmp(name, "rgb(", 4)) {
        int r, g, b;
        if (sscanf(name + 4, "%d , %d , %d", &r, &g, &b) == 3) {
            rgb[0] = r;
            rgb[1] = g;
            rgb[2] = b;
            return 0;
        }
    }
    return -1;
}

/* 解析一条声明列表(不含 selector, 即 "{ prop:val; ... }" 去掉花括号)
 * 就地修改 buf */
static void parse_decls(char *decls, struct cstyle *s)
{
    char *save = decls;
    int guard = 0;
    while (*save && guard++ < 128) {
        char *semi = strchr(save, ';');
        char *chunk = save;
        if (semi)
            *semi = 0;
        char *kv = chunk;
        while (*kv == ' ' || *kv == '\t' || *kv == '\n')
            kv++;
        char *colon = strchr(kv, ':');
        if (!colon) {
            if (!semi)
                break;
            save = semi + 1;
            continue;
        }
        *colon = 0;
        char *key = chunk, *val = colon + 1;
        while (*key == ' ' || *key == '\t')
            key++;
        char *ek = key + strlen(key);
        while (ek > key && (ek[-1] == ' ' || ek[-1] == '\t'))
            *--ek = 0;
        while (*val == ' ' || *val == '\t')
            val++;
        char *ev = val + strlen(val);
        while (ev > val && (ev[-1] == ' ' || ev[-1] == '\t' || ev[-1] == '\n'))
            *--ev = 0;
        char *unit = strstr(val, "px");
        if (unit)
            *unit = 0;
        if (strcasecmp(key, "display") == 0 &&
            strcasecmp(val, "none") == 0)
            s->disp_none = 1;
        else if (strcasecmp(key, "font-weight") == 0 &&
                 (strcasecmp(val, "bold") == 0 ||
                  strcasecmp(val, "bolder") == 0 ||
                  strcasecmp(val, "600") == 0 ||
                  strcasecmp(val, "700") == 0 ||
                  strcasecmp(val, "800") == 0 ||
                  strcasecmp(val, "900") == 0))
            s->bold = 1;
        else if (strcasecmp(key, "font-style") == 0 &&
                 strcasecmp(val, "italic") == 0)
            s->italic = 1;
        else if (strcasecmp(key, "color") == 0)
            css_color(val, s->fg);
        else if (strcasecmp(key, "background-color") == 0 ||
                 strcasecmp(key, "background") == 0) {
            char *ws = strpbrk(val, " \t(");
            if (ws)
                *ws = 0;
            css_color(val, s->bg);
        } else if (strcasecmp(key, "font-size") == 0) {
            int fs = atoi(val);
            if (fs > s->fontsize)
                s->fontsize = fs;
        }
        if (!semi)
            break;
        save = semi + 1;
    }
}

/* 从 "<style>...内容..." 中逐条解析规则 */
static void parse_css_block(const char *s, int send_off)
{
    const char *send = s + send_off;
    const char *q = s;
    int guard = 0;
    while (q < send && guard++ < 512) {
        const char *brk = memchr(q, '{', (size_t)(send - q));
        if (!brk)
            break;
        const char *cl = memchr(brk + 1, '}', (size_t)(send - brk - 1));
        if (!cl)
            break;
        /* selector = q..brk, decls = brk+1..cl */
        char sel[64], decls[512];
        size_t sl = (size_t)(brk - q);
        while (sl && isspace((unsigned char)q[sl - 1]))
            sl--;
        /* 跳过前导空白 */
        const char *sel0 = q;
        while ((const char *)sel0 - q < sl &&
               isspace((unsigned char)*sel0))
            sel0++;
        sl -= (size_t)(sel0 - q);
        sl = sl < 63 ? sl : 63;
        memcpy(sel, sel0, sl);
        sel[sl] = 0;
        /* 逗号分隔多选择器: 只保留最后一段 */
        char *cm = strchr(sel, ',');
        if (cm) {
            char *seg = cm + 1;
            while (*seg == ' ' || *seg == '\t')
                seg++;
            sl = strnlen(seg, 63);
            memmove(sel, seg, sl + 1);
        }
        size_t dl = (size_t)(cl - brk - 1);
        if (dl >= sizeof decls)
            dl = sizeof decls - 1;
        memcpy(decls, brk + 1, dl);
        decls[dl] = 0;
        if (g_nrules < CSS_RULE_CAP) {
            struct css_rule *r = &g_rules[g_nrules];
            r->selector[0] = 0;
            memcpy(r->selector, sel, sl < 63 ? sl : 63);
            r->selector[sl < 63 ? sl : 63] = 0;
            r->st = empty_style();
            parse_decls(decls, &r->st);
            g_nrules++;
        }
        q = cl + 1;
    }
}

/* 规则选择器匹配节点; 返回 0 = 不匹配, 1 = 通配, 2 = 标签,
 * 3 = 类, 4 = id, 5 = 复合(标签+类/id) */
static int match_rule(const struct css_rule *r, const struct node *n)
{
    const char *sel = r->selector;
    if (strcmp(sel, "*") == 0)
        return 1;
    if (sel[0] == '#')
        return (n->id[0] &&
                strcasecmp(n->id, sel + 1) == 0) ? 4 : 0;
    if (sel[0] == '.')
        return (n->cls[0] &&
                strcasecmp(n->cls, sel + 1) == 0) ? 3 : 0;
    /* 可能是 tag / tag.cls / tag#id */
    char t[32] = {0};
    const char *dot = strchr(sel, '.');
    const char *hash = strchr(sel, '#');
    const char *sep = dot;
    if (hash && (!dot || hash < dot))
        sep = hash;
    size_t tl = sep ? (size_t)(sep - sel) : strnlen(sel, 31);
    tl = tl < 31 ? tl : 31;
    memcpy(t, sel, tl);
    t[tl] = 0;
    if (strcasecmp(n->tag, t) != 0)
        return 0;
    if (sep) {
        const char *u = sep + 1;
        if (sep[0] == '.' && n->cls[0] &&
            strcasecmp(n->cls, u) == 0)
            return 5;
        if (sep[0] == '#' && n->id[0] &&
            strcasecmp(n->id, u) == 0)
            return 5;
        return 0;
    }
    return 2;
}

/* 合并: 高优先级 src 覆盖 dst(仅覆盖 src 已设的属性) */
static void merge_style(struct cstyle *dst, const struct cstyle *src)
{
    for (int i = 0; i < 3; i++)
        if (src->fg[i] >= 0)
            dst->fg[i] = src->fg[i];
    for (int i = 0; i < 3; i++)
        if (src->bg[i] >= 0)
            dst->bg[i] = src->bg[i];
    if (src->bold)
        dst->bold = 1;
    if (src->italic)
        dst->italic = 1;
    if (src->fontsize > dst->fontsize)
        dst->fontsize = src->fontsize;
    if (src->disp_none)
        dst->disp_none = 1;
}

/* 对节点计算样式: 收集所有匹配规则(按优先级 id>cls>tag>*),
 * 再加内联(最高), 低优先级先合入, 高优先级后覆盖 */
static void compute_styles(struct page *pg, int idx, struct cstyle *out)
{
    struct node *n = &pg->nodes[idx];
    /* 桶: 0=* / 1=标签 / 2=类(含复合) / 3=id / 4=内联 */
    struct cstyle bucket[5];
    for (int b = 0; b < 5; b++)
        bucket[b] = empty_style();
    int have[5] = {0};
    for (int i = 0; i < g_nrules; i++) {
        int m = match_rule(&g_rules[i], n);
        if (!m)
            continue;
        int b = m == 1 ? 0 : m == 2 ? 1 : m == 3 ? 2 : m == 4 ? 3 : 2;
        merge_style(&bucket[b], &g_rules[i].st);
        have[b] = 1;
    }
    /* 内联 style 属性(最高优先级) */
    if (n->style[0]) {
        char sbuf[256];
        size_t l = strnlen(n->style, sizeof sbuf - 1);
        memcpy(sbuf, n->style, l);
        sbuf[l] = 0;
        parse_decls(sbuf, &bucket[4]);
        have[4] = 1;
    }
    *out = empty_style();
    for (int b = 0; b < 5; b++)
        if (have[b])
            merge_style(out, &bucket[b]);
}

/* 递归遍历 DOM 计算每节点样式 */
static void compute_all(struct page *pg, int idx)
{
    struct cstyle cs;
    compute_styles(pg, idx, &cs);
    pg->nodes[idx].comp = alloc_comp(pg, &cs);
    for (int c = pg->nodes[idx].first; c != -1;
         c = pg->nodes[c].next)
        compute_all(pg, c);
}

/* ================= HTML 建树 ================= */
static void parse_html(struct page *pg, const char *html, size_t len)
{
    int stack[64];
    int top = 0;
    int root = node_new(pg, "#root", 0);
    pg->root = root;
    stack[top++] = root;
    char text[TXT_CAP];
    size_t tlen = 0;

    /* 把累积文本作为子节点挂到栈顶元素下 */
    #define FLUSH_TEXT() do { \
        if (tlen) { \
            char dec[TXT_CAP]; \
            size_t dl = decode_entities(dec, text, tlen); \
            dec[dl] = 0; \
            int ti = node_new(pg, "", 1); \
            if (ti >= 0 && top) { \
                pg->nodes[ti].text = strdup(dec); \
                node_add_child(pg, stack[top - 1], ti); \
            } \
            tlen = 0; \
        } \
    } while (0)

    const char *p = html;
    const char *pend = html + len;
    long iters = 0, itcap = (long)len * 4 + 2048;
    while (p < pend) {
        if (++iters > itcap)
            break;
        if (*p != '<') {
            if (tlen < TXT_CAP - 1)
                text[tlen++] = *p;
            p++;
            continue;
        }
        const char *gt = memchr(p, '>', (size_t)(pend - p));
        if (!gt)
            break;
    /* 只接受 '<' 后紧跟 '/' 或字母 才是标签, 排掉实体 (&lt; 等) */
        {
            const char *q = p + 1;
            while (*q && isspace((unsigned char)*q))
                q++;
            if (!(*q == '/' || isalpha((unsigned char)*q))) {
                /* 不是标签: 整段(含实体)作为文本保留 */
                size_t el = (size_t)(gt - p + 1);
                if (tlen < TXT_CAP - 1) {
                    el = el < (size_t)(TXT_CAP - 1 - tlen) ? el
                                                           : (size_t)(TXT_CAP - 1 - tlen);
                    memcpy(text + tlen, p, el);
                    tlen += el;
                }
                p = gt + 1;
                continue;
            }
        }
        char tag[256];
        size_t tl = (size_t)(gt - p - 1);
        tl = tl < 255 ? tl : 255;
        memcpy(tag, p + 1, tl);
        tag[tl] = 0;

        /* 取标签名(开标签可能带 '/' 自闭合; 闭标签是 /name) */
        char tname[32] = {0};
        size_t tn = 0;
        const char *s = p + 1;
        while (*s == ' ' || *s == '\n' || *s == '\t')
            s++;
        int is_close = (*s == '/');
        if (is_close)
            s++;
        while (*s && !isspace((unsigned char)*s) && *s != '/' &&
               *s != '>' && tn < 31)
            tname[tn++] = tolower((unsigned char)*s++);
        tname[tn] = 0;

        /* 弹栈到匹配的开标签(容错: 找得到就截断, 找不到忽略);
           自闭合标签的幻影栈项直接出栈 */
        if (is_close) {
            /* 闭合标签前先把累积文本挂到当前栈顶(如 <li> 内文本在 </li>
               才属于该 li), 再弹栈 */
            FLUSH_TEXT();
            for (int k = top - 1; k >= 0; k--) {
                if (!strcasecmp(tname, pg->nodes[stack[k]].tag)) {
                    top = k;
                    break;
                }
            }
            p = gt + 1;
            continue;
        }

        /* <style>: 内容交给 CSS 解析器, 不挂 DOM;
           结束位置跳到 </style> 的 '>', 不重读闭标签 */
        if (!strcasecmp(tname, "style")) {
            const char *se = strstr(gt + 1, "</style");
            const char *end = se ? se : pend;
            parse_css_block(gt + 1, (int)(end - (gt + 1)));
            if (se) {
                const char *gt2 = memchr(se, '>', (size_t)(pend - se));
                p = gt2 ? gt2 + 1 : pend;
            } else
                p = pend;
            continue;
        }
        /* <script>: 整块丢弃 */
        if (!strcasecmp(tname, "script")) {
            const char *se = strstr(gt + 1, "</script");
            if (se) {
                const char *gt2 = memchr(se, '>', (size_t)(pend - se));
                p = gt2 ? gt2 + 1 : pend;
            } else
                p = pend;
            continue;
        }

        FLUSH_TEXT();

        int ni = node_new(pg, tname, 0);
        if (ni < 0) {
            p = gt + 1;
            continue;
        }
        struct node *n = &pg->nodes[ni];

        /* <title>: 文本不进正文, 记为页面标题;
           结束位置须跳过 </title> 的 '>' */
        if (!strcasecmp(tname, "title")) {
            const char *se = strstr(gt + 1, "</title");
            const char *he = strstr(gt + 1, "</head");
            if (he && (!se || se > he))
                se = he;
            const char *ss = gt + 1;
            if (se && se > ss) {
                size_t l = (size_t)(se - ss);
                l = l < 255 ? l : 255;
                char dec[TXT_CAP];
                size_t dl = decode_entities(dec, ss, l);
                dec[dl] = 0;
                /* 折叠空白 */
                char *w = dec, *d = dec;
                int sp = 1;
                while (*w) {
                    if (isspace((unsigned char)*w)) {
                        if (!sp) *d++ = ' ';
                        sp = 1;
                    } else {
                        *d++ = *w;
                        sp = 0;
                    }
                    w++;
                }
                *d = 0;
                free(pg->title);
                pg->title = strdup(dec);
            }
            /* se 指向 '</title' 或 '</head' 起始, 跳至该闭标签的 '>';
               找不到则仅跳过本开标签 */
            if (se) {
                const char *gt2 = memchr(se, '>', (size_t)(pend - se));
                p = gt2 ? gt2 + 1 : pend;
            } else {
                p = gt + 1;
            }
            continue;
        }

        /* 属性: class/id/href/style */
        char val[512];
        if (tag_attr(tag, "class", val, sizeof val))
            copy_str(n->cls, val, sizeof n->cls);
        if (tag_attr(tag, "id", val, sizeof val))
            copy_str(n->id, val, sizeof n->id);
        if (tag_attr(tag, "href", val, sizeof val))
            copy_str(n->href, val, sizeof n->href);
        if (tag_attr(tag, "style", val, sizeof val))
            copy_str(n->style, val, sizeof n->style);
        /* <img>: alt 存到 href 复用字段(渲染层按标签区分语义) */
        if (!strcasecmp(tname, "img") &&
            tag_attr(tag, "alt", val, sizeof val))
            copy_str(n->href, val, sizeof n->href);

        /* <a>: 登记链接目标(alt[0] 存链接序号, 渲染时回填文本) */
        if (!strcasecmp(tname, "a") && n->href[0]) {
            if (pg->nlinks < LINK_CAP) {
                n->alt[0] = (char)(unsigned char)pg->nlinks;
                n->alt[1] = 0;
                strncpy(pg->links[pg->nlinks].target, n->href,
                        sizeof pg->links[0].target - 1);
                pg->nlinks++;
            }
        }

        /* <img>: 子节点挂 [IMG alt] 占位文本 */
        if (!strcasecmp(tname, "img")) {
            int ti = node_new(pg, "", 1);
            if (ti >= 0) {
                char ph[160];
                snprintf(ph, sizeof ph, "[IMG%s]",
                         n->href[0] ? n->href : "");
                pg->nodes[ti].text = strdup(ph);
                node_add_child(pg, ni, ti);
            }
        }

        /* <hr>: 分隔线由渲染层处理, 不挂文本哨兵 */

        if (!is_self_closing(tname) && top < 64) {
            node_add_child(pg, stack[top - 1], ni);
            stack[top++] = ni;
        } else if (top) {
            node_add_child(pg, stack[top - 1], ni);
        }

        p = gt + 1;
    }
    FLUSH_TEXT();
    #undef FLUSH_TEXT
}

/* ================= 取页 ================= */
static int fetch_page(const char *url, char *out, size_t cap)
{
    if (url[0] == 'f' && !strncasecmp(url, "file://", 7)) {
        FILE *f = fopen(url + 7, "rb");
        if (!f) {
            fprintf(stderr, "打不开本地文件: %s\n", url + 7);
            return -1;
        }
        size_t n = fread(out, 1, cap - 1, f);
        out[n] = 0;
        fclose(f);
        return (int)n;
    }
    if (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) {
        char tmpf[64];
        snprintf(tmpf, sizeof tmpf, "/tmp/w3m-%d.html", (int)getpid());
        struct http_options opt;
        memset(&opt, 0, sizeof opt);
        opt.url = url;
        opt.output = tmpf;
        opt.follow = 1;        /* 跟 3xx 重定向 */
        opt.timeout = 30;
        struct http_result res;
        int rc = http_download(&opt, &res, NULL);
        if (rc) {
            fprintf(stderr, "抓取失败: %s (HTTP %d)\n", res.error, res.status);
            return -1;
        }
        FILE *f = fopen(tmpf, "rb");
        if (!f) {
            unlink(tmpf);
            fprintf(stderr, "读回抓取内容失败\n");
            return -1;
        }
        size_t n = fread(out, 1, cap - 1, f);
        out[n] = 0;
        fclose(f);
        unlink(tmpf);
        return (int)n;
    }
    FILE *f = fopen(url, "rb");
    if (!f) {
        fprintf(stderr, "打不开 %s\n", url);
        return -1;
    }
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
    return (int)n;
}
/* ================= 终端渲染 ================= */

/* 终端宽度: COLUMNS 环境变量 > isatty 查 winsize > 80 默认 */
static int term_width(void)
{
    const char *c = getenv("COLUMNS");
    if (c && *c) {
        int w = atoi(c);
        if (w >= 40)
            return w;
    }
    struct winsize ws;
    if (isatty(1) && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* 折叠空白: 连续空白折成单空格, 去首尾; pre 场景原样返回 */
static void collapse_ws(const char *s, char *buf, size_t cap, int in_pre)
{
    if (in_pre) {
        size_t l = strnlen(s, cap - 1);
        memcpy(buf, s, l);
        buf[l] = 0;
        return;
    }
    size_t o = 0;
    int sp = 0;
    while (*s) {
        if (isspace((unsigned char)*s)) {
            if (!sp && o + 1 < cap)
                buf[o++] = ' ';
            sp = 1;
        } else {
            if (o + 1 < cap)
                buf[o++] = *s;
            sp = 0;
        }
        s++;
    }
    while (o && buf[o - 1] == ' ')
        buf[--o] = 0;
    buf[o] = 0;
}

/* UTF-8 字符宽度(CJK 计 2 列); adv = 字节步长 */
static int utf8_w(const char *s, int *adv)
{
    unsigned char c = (unsigned char)*s;
    if (c < 0x80) { *adv = 1; return 1; }
    if (c < 0xc0) { *adv = 0; return 0; }
    int n = 2;
    if (c >= 0xe0 && c < 0xf0)
        n = 3;
    else if (c >= 0xf0)
        n = 4;
    for (int i = 1; i < n; i++)
        if ((unsigned char)s[i] < 0x80 || (unsigned char)s[i] >= 0xc0)
            n = i;
    *adv = n;
    return c >= 0x80 ? 2 : 1;
}

/* 聚合节点子树的全部文本(折叠空白后) */
static void gather_text(struct page *pg, int idx, char *out, size_t cap)
{
    struct node *n = &pg->nodes[idx];
    if (n->is_text) {
        char dec[8192];
        collapse_ws(n->text ? n->text : "", dec, sizeof dec, 0);
        size_t l = strnlen(dec, cap - 1);
        memcpy(out, dec, l);
        out[l] = 0;
        return;
    }
    out[0] = 0;
    size_t o = 0;
    for (int c = n->first; c != -1; c = pg->nodes[c].next) {
        char tmp[512];
        gather_text(pg, c, tmp, sizeof tmp);
        size_t tl = strlen(tmp);
        if (o + tl + 1 < cap) {
            memcpy(out + o, tmp, tl);
            o += tl;
            out[o] = 0;
        }
    }
}

/* 行内流渲染上下文(须先于 struct inl 定义) */
struct ctx {
    struct page *pg;
    int width;
    int t24;
};

/* 行内流渲染状态 */
struct inl {
    struct ctx *cx;
    int at;      /* 当前列 */
};

/* 输出字符串(计入 at), 超宽自动换行 */
static void inl_put(struct inl *st, const char *s)
{
    struct ctx *cx = st->cx;
    for (; *s; s++) {
        int adv = 0, w = utf8_w(s, &adv);
        if (w == 0)
            continue;
        if (st->at + w > cx->width - 1) {
            fputc('\n', stdout);
            st->at = 0;
        }
        for (int i = 0; i < adv; i++)
            fputc((unsigned char)s[i], stdout);
        st->at += w;
    }
}

static void inl_space(struct inl *st)
{
    st->at++;
    if (st->at >= st->cx->width) {
        fputc('\n', stdout);
        st->at = 0;
    } else
        fputc(' ', stdout);
}

static void inl_newline(struct inl *st)
{
    fputc('\n', stdout);
    st->at = 0;
}

/* 把计算样式发成 ANSI 前缀, 返回长度(0 = 无样式) */
static size_t ansi_prefix(char *buf, size_t cap, const struct cstyle *s,
                         int t24)
{
    if (!s)
        return 0;
    int any = s->bold || s->italic || s->fg[0] >= 0 || s->bg[0] >= 0 ||
              s->fontsize > 16;
    if (!any)
        return 0;
    size_t o = 0;
    o += snprintf(buf + o, cap, "\033[");
    int first = 1;
    if (s->bold) {
        o += snprintf(buf + o, cap - o, "1");
        first = 0;
    }
    if (s->italic) {
        if (!first)
            o += snprintf(buf + o, cap - o, ";");
        o += snprintf(buf + o, cap - o, "3");
        first = 0;
    }
    if (s->fg[0] >= 0) {
        if (!first)
            o += snprintf(buf + o, cap - o, ";");
        if (t24) {
            o += snprintf(buf + o, cap - o, "38;2;%d;%d;%d",
                          s->fg[0], s->fg[1], s->fg[2]);
        } else {
            int r = s->fg[0] > 127, g = s->fg[1] > 127, b = s->fg[2] > 127;
            static const int base[] = {30, 31, 32, 33, 34, 35, 36, 37};
            o += snprintf(buf + o, cap - o, "%d",
                          base[(r << 2) | (g << 1) | b]);
        }
        first = 0;
    }
    if (s->bg[0] >= 0) {
        if (!first)
            o += snprintf(buf + o, cap - o, ";");
        o += snprintf(buf + o, cap - o, "48;2;%d;%d;%d",
                      s->bg[0], s->bg[1], s->bg[2]);
        first = 0;
    }
    if (s->fontsize > 16) {
        if (!first)
            o += snprintf(buf + o, cap - o, ";");
        o += snprintf(buf + o, cap - o, "9");
    }
    o += snprintf(buf + o, cap - o, "m");
    return o;
}

/* 渲染 DOM 节点(行内流 + 块级换行) */
static void render(struct ctx *cx, int idx, struct inl *st);

/* <li>: 前缀 + 聚合文本 + 嵌套列表递归 */
static void render_li(struct ctx *cx, int idx, int counter, int ordered,
                      struct inl *st)
{
    struct page *pg = cx->pg;
    struct node *n = &pg->nodes[idx];
    if (n->comp >= 0 && pg->comp[n->comp].disp_none)
        return;
    const struct cstyle *s =
        n->comp >= 0 ? &pg->comp[n->comp] : NULL;
    char esc[192];
    size_t el = ansi_prefix(esc, sizeof esc, s, cx->t24);
    inl_newline(st);
    inl_put(st, "  ");
    if (el)
        printf("%.*s", (int)el, esc);
    char prefix[32];
    snprintf(prefix, sizeof prefix, ordered ? "%d. " : "- ", counter);
    inl_put(st, prefix);
    /* 聚合直接子文本(文本子直接拼; 元素子走 gather_text; 嵌套 ul/ol 单独递归) */
    char txt[4096];
    txt[0] = 0;
    for (int c = n->first; c != -1; c = pg->nodes[c].next) {
        struct node *cn = &pg->nodes[c];
        if (cn->is_text) {
            strncat(txt, cn->text ? cn->text : "",
                    sizeof txt - strlen(txt) - 1);
            continue;
        }
        if (strcasecmp(cn->tag, "ul") && strcasecmp(cn->tag, "ol")) {
            char tmp[1024];
            gather_text(pg, c, tmp, sizeof tmp);
            strncat(txt, tmp, sizeof txt - strlen(txt) - 1);
        }
    }
    inl_put(st, txt);
    if (el)
        printf("\033[0m");
    /* 嵌套列表: 前缀加 2 空格 */
    int cnt = 0;
    for (int c = n->first; c != -1; c = pg->nodes[c].next) {
        struct node *cn = &pg->nodes[c];
        if (!strcasecmp(cn->tag, "ul") || !strcasecmp(cn->tag, "ol")) {
            int ord = !strcasecmp(cn->tag, "ol");
            for (int d = cn->first; d != -1; d = pg->nodes[d].next)
                if (!strcasecmp(pg->nodes[d].tag, "li")) {
                    inl_newline(st);
                    inl_put(st, "    ");
                    render_li(cx, d, ++cnt, ord, st);
                }
        }
    }
}

/* 渲染 DOM 节点(行内流 + 块级换行) */
static void render(struct ctx *cx, int idx, struct inl *st)
{
    struct page *pg = cx->pg;
    struct node *n = &pg->nodes[idx];
    if (n->comp >= 0 && pg->comp[n->comp].disp_none)
        return;
    const struct cstyle *s =
        n->comp >= 0 ? &pg->comp[n->comp] : NULL;
    const char *t = n->tag;

    if (n->is_text) {
        inl_put(st, n->text ? n->text : "");
        return;
    }

    /* 结构标签: 直接递归 */
    if (!strcasecmp(t, "html") || !strcasecmp(t, "body") ||
        !strcasecmp(t, "head") || !strcasecmp(t, "#root")) {
        for (int c = n->first; c != -1; c = pg->nodes[c].next)
            render(cx, c, st);
        return;
    }

    /* 列表容器: 直接子 li 按文档序渲染(错位容错: li 的 parent 可能挂在
       别的祖先上, 只要 parent 索引 > 本容器索引就按创建序补渲染) */
    if (!strcasecmp(t, "ul") || !strcasecmp(t, "ol")) {
        int ordered = !strcasecmp(t, "ol");
        int counter = 0;
        for (int c = n->first; c != -1; c = pg->nodes[c].next)
            if (!strcasecmp(pg->nodes[c].tag, "li"))
                render_li(cx, c, ++counter, ordered, st);
        if (counter == 0) {
            /* 兜底: 创建序扫描 —— 容器之后、且 parent 索引不小于容器索引的
               li, 视为本列表错位挂出的子项 */
            for (int i = idx + 1; i < pg->nnodes; i++) {
                if (strcasecmp(pg->nodes[i].tag, "li"))
                    continue;
                if (pg->nodes[i].parent < idx)
                    break;
                render_li(cx, i, ++counter, ordered, st);
            }
        }
        inl_newline(st);
        return;
    }

    if (!strcasecmp(t, "li")) {
        render_li(cx, idx, 1, 0, st);
        return;
    }

    /* 标题: 前后空行 + 粗体 + 字号标注 */
    if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0) {
        inl_newline(st);
        char esc[192];
        size_t el = ansi_prefix(esc, sizeof esc, s, cx->t24);
        if (el)
            printf("%.*s", (int)el, esc);
        char txt[4096];
        gather_text(pg, idx, txt, sizeof txt);
        inl_put(st, txt);
        int lvl = t[1] - '0';
        if (lvl == 1)
            inl_put(st, "  [大]");
        else if (lvl == 2)
            inl_put(st, "  [中]");
        if (el)
            printf("\033[0m");
        inl_newline(st);
        inl_newline(st);
        return;
    }

    /* 段落 */
    if (!strcasecmp(t, "p")) {
        inl_newline(st);
        char esc[192];
        size_t el = ansi_prefix(esc, sizeof esc, s, cx->t24);
        if (el)
            printf("%.*s", (int)el, esc);
        char txt[4096];
        gather_text(pg, idx, txt, sizeof txt);
        inl_put(st, txt);
        if (el)
            printf("\033[0m");
        inl_newline(st);
        inl_newline(st);
        return;
    }

    /* 分隔线 */
    if (!strcasecmp(t, "hr")) {
        inl_newline(st);
        inl_put(st, "----------------------------------------");
        inl_newline(st);
        return;
    }

    /* 预格式: 保留空白, 逐行缩进 2 */
    if (!strcasecmp(t, "pre")) {
        inl_newline(st);
        for (int c = n->first; c != -1; c = pg->nodes[c].next) {
            struct node *cn = &pg->nodes[c];
            if (cn->is_text && cn->text) {
                inl_put(st, "  ");
                inl_put(st, cn->text);
                inl_newline(st);
            }
        }
        inl_newline(st);
        return;
    }

    /* 表格行: 各单元格 " | " 分隔 */
    if (!strcasecmp(t, "tr")) {
        inl_newline(st);
        inl_put(st, "  ");
        int first_cell = 1;
        for (int c = n->first; c != -1; c = pg->nodes[c].next) {
            if (!first_cell)
                inl_put(st, " | ");
            first_cell = 0;
            char cell[1024];
            gather_text(pg, c, cell, sizeof cell);
            inl_put(st, cell);
        }
        inl_newline(st);
        return;
    }

    /* <a>: 行内, 聚合文本 + 回填链接表 */
    if (!strcasecmp(t, "a") && n->href[0]) {
        char txt[4096];
        gather_text(pg, idx, txt, sizeof txt);
        char esc[192];
        size_t el = ansi_prefix(esc, sizeof esc, s, cx->t24);
        if (el)
            printf("%.*s", (int)el, esc);
        inl_put(st, txt);
        if (el)
            printf("\033[0m");
        int li = (int)(unsigned char)n->alt[0];
        if (li < pg->nlinks)
            strncpy(pg->links[li].text, txt,
                    sizeof pg->links[li].text - 1);
        inl_space(st);
        return;
    }

    /* div / blockquote: 块级, 前后空行 */
    if (!strcasecmp(t, "div") || !strcasecmp(t, "blockquote")) {
        inl_newline(st);
        char esc[192];
        size_t el = ansi_prefix(esc, sizeof esc, s, cx->t24);
        if (el)
            printf("%.*s", (int)el, esc);
        for (int c = n->first; c != -1; c = pg->nodes[c].next)
            render(cx, c, st);
        if (el)
            printf("\033[0m");
        inl_newline(st);
        inl_newline(st);
        return;
    }

    /* br */
    if (!strcasecmp(t, "br")) {
        inl_newline(st);
        return;
    }

    /* 其他行内(b/i/span/code/td/th/table): 聚合文本, 样式由本节点承载 */
    {
        char txt[4096];
        gather_text(pg, idx, txt, sizeof txt);
        if (!txt[0])
            return;
        char esc[192];
        size_t el = ansi_prefix(esc, sizeof esc, s, cx->t24);
        if (el)
            printf("%.*s", (int)el, esc);
        inl_put(st, txt);
        if (el)
            printf("\033[0m");
        inl_space(st);
    }
}

/* 渲染整页 */
static void print_page(struct page *pg, const char *url)
{
    if (pg->title)
        printf("标题: %s\n", pg->title);
    printf("来源: %s\n\n", url);
    struct ctx cx = { pg, term_width(), isatty(1) ? 1 : 0 };
    struct inl st = { &cx, 0 };
    render(&cx, pg->root, &st);
    fputc('\n', stdout);
    printf("--- 链接表(%d 条) ---\n", pg->nlinks);
    for (int i = 0; i < pg->nlinks; i++)
        printf(" [%d] %s -> %s\n", i + 1,
               pg->links[i].text[0] ? pg->links[i].text : "(无文字)",
               pg->links[i].target);
}

int main(int argc, char **argv)
{
    const char *url = NULL;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-')
            url = argv[i];
    if (!url) {
        printf("w3m [URL|文件]\n");
        printf("  支持本地路径 / file:// / http(s)://(自动跟 3xx 重定向)\n");
        printf("  标签: html/body/title h1~h6 p br hr div span a[href]\n"
               "        ul/ol/li pre b/strong i/em img style/script\n");
        printf("  实体: &lt; &gt; &amp; &quot; &nbsp; &#NNN;\n");
        printf("  CSS: display:none color background-color\n"
               "       font-weight:bold font-size  选择器 * / 标签 / .类 / #id / 标签.类\n");
        return 1;
    }

    char *buf = malloc(PAGE_BUF_CAP);
    if (!buf)
        return 1;
    int n = fetch_page(url, buf, PAGE_BUF_CAP);
    if (n < 0) {
        free(buf);
        return 1;
    }

    g_nrules = 0;
    struct page *pg = page_new();
    parse_html(pg, buf, (size_t)n);
    free(buf);
    compute_all(pg, pg->root);
    print_page(pg, url);
    page_free(pg);
    return 0;
}
