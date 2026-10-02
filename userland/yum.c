// SPDX-License-Identifier: GPL-2.0-or-later
/* yum.c - Parlz 移植的 yum/dnf 风格前端(repodata → 求解 → 下载 → 交给 rpm)。
 *
 * 分工与上游一致: yum 只管"仓库配置 → repodata → 依赖求解 → 下载校验",
 * 真正解包安装一律 fork+execv 调 /bin/rpm(本仓库的移植实现)。
 *
 * 仓库配置: /etc/yum.repos.d/*.repo (INI 风格; 文件名排序后依次读)
 *     [id]
 *     name=随便
 *     baseurl=http://host/path     ($releasever / $basearch 会替换)
 *     enabled=1|0
 *     gpgcheck=1|0
 * 索引: <baseurl>/repodata/repomd.xml → <data type="primary"> 里的
 *     <checksum type="sha256"> 与 <location href="repodata/....xml.gz"/>;
 *   下载后**先按 repomd 声明的摘要校**(压缩态), 再 gunzip 成 primary.xml,
 *   缓存到 /var/cache/yum/<id>/primary.xml。
 *   primary.xml 每个 <package type="rpm"> 里有 name/arch/
 *   version(epoch ver rel)/checksum(pkgid)/size(package)/location(href)/
 *   format 里的 rpm:requires、rpm:provides 与 <file> 列表。
 *   (结构与上游一致 —— 判据用宿主 createrepo_c 生成的真仓库核,
 *    见 scripts/yum-verify.sh)
 *
 * 动作:
 *   yum makecache              拉所有启用仓库的 repodata 并缓存
 *   yum install <包>...         求解依赖 → 下载 → 校验 → rpm -i
 *   yum update <包>...          同上但 rpm -U(没有包名时等于 makecache)
 *   yum remove|erase <包>...     rpm -e
 *   yum list [installed|available] [模式]
 *   yum info <包>...  yum search <关键字>...  yum repolist  yum clean
 * 选项:
 *   --installroot <目录> --rpm <路径> -y|--assumeyes --nogpgcheck
 *   --skip-broken --releasever=<值> --disablerepo=<id> --enablerepo=<id>
 *
 * 安全/保真边界(说清楚, 不冒充完整的 yum):
 *   - **不导公钥环、不验包签名**。仓库 gpgcheck=1(或没写, 默认就是 1)时
 *     拒绝安装, 必须显式 gpgcheck=0 或 --nogpgcheck —— 这道闸门替代签名校验。
 *   - 索引与包体都按 sha256 校; 仓库声明成 sha1/md5 时直接拒绝,
 *     不做"算不了就当通过"。
 *   - rpmlib(...) 是"包管理器自带能力", 按已满足处理(上游也这样)。
 *   - 以 '/' 开头的依赖按**文件**判断: 装出来的文件已存在于 root 下就算满足,
 *     否则去索引里找哪个包的 <file> 列表提供它。
 *   - erase **不查反向依赖**: 本实现的 rpm 状态库没记 requires,
 *     所以做不到"还有别人依赖它就不许删"(apt/dpkg 那条路有, 因为
 *     dpkg 的 status 里有 Depends)。
 *   - 没有事务回滚、没有插件、没有 group/module、不读 *_db 那种 sqlite 索引。
 * 约束(AGENTS.md): 静态、无 system()、全 fork+execv。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "pkgcore.h"
#include "http_client.h"

#define YUM_VERSION   "1.0.0-parlz"
#define MAX_REPO      16
#define MAX_PKG       1000
#define MAX_REQ       16
#define MAX_PRV       16
#define MAX_FILE      12
#define XML_MAX_BYTES (48L * 1024 * 1024)

static char opt_root[512];                 /* --installroot */
static char rpm_path[256] = "/bin/rpm";
static int opt_yes, opt_nogpg, opt_skip_broken;
static char opt_releasever[64] = "9";
static char opt_disablerepo[64], opt_enablerepo[64];

static const char *host_basearch(void)
{
    static char a[32];
    if (a[0])
        return a;
    struct utsname u;
    if (uname(&u) == 0) {
        if (!strcmp(u.machine, "x86_64"))
            snprintf(a, sizeof a, "x86_64");
        else if (!strcmp(u.machine, "i686") || !strcmp(u.machine, "i386"))
            snprintf(a, sizeof a, "i386");
        else if (!strcmp(u.machine, "aarch64"))
            snprintf(a, sizeof a, "aarch64");
        else
            snprintf(a, sizeof a, "%s", u.machine);
    } else
        snprintf(a, sizeof a, "x86_64");
    return a;
}

/* ------------------------- 路径 ------------------------- */
static void p_reposd(char *out, size_t n)
{
    const char *e = getenv("YUM_REPOS_DIR");
    if (e && *e) {
        snprintf(out, n, "%s", e);
        return;
    }
    snprintf(out, n, "%s/etc/yum.repos.d", opt_root);
}
static void p_cache(char *out, size_t n)
{
    snprintf(out, n, "%s/var/cache/yum", opt_root);
}
static void p_rpmdb(char *out, size_t n)
{
    snprintf(out, n, "%s/var/lib/rpm/installed", opt_root);
}

/* ------------------------- 仓库配置 ------------------------- */
struct repo {
    char id[64];
    char name[128];
    char base[256];
    int enabled;
    int gpgcheck;
};
static struct repo repos[MAX_REPO];
static int nrepo;

static void rstrip(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == ' ' || s[l - 1] == '\t' || s[l - 1] == '\r' ||
                 s[l - 1] == '\n'))
        s[--l] = 0;
}

/* $releasever / $basearch 替换 */
static void expand_vars(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n;) {
        if (*p == '$' && !strncmp(p, "$releasever", 11)) {
            int w = snprintf(out + o, n - o, "%s", opt_releasever);
            if (w < 0)
                break;
            o += (size_t)w;
            p += 11;
        } else if (*p == '$' && !strncmp(p, "$basearch", 9)) {
            int w = snprintf(out + o, n - o, "%s", host_basearch());
            if (w < 0)
                break;
            o += (size_t)w;
            p += 9;
        } else
            out[o++] = *p++;
    }
    out[o] = 0;
}

static int repo_parse_file(const char *fp)
{
    FILE *f = fopen(fp, "r");
    if (!f)
        return -1;
    char line[512];
    struct repo *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#' || *s == ';')
            continue;
        rstrip(s);
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (!e)
                continue;
            if (nrepo >= MAX_REPO) {
                fprintf(stderr, "yum: 仓库数超过上限 %d, 其余忽略\n", MAX_REPO);
                cur = NULL;
                continue;
            }
            cur = &repos[nrepo++];
            memset(cur, 0, sizeof *cur);
            cur->enabled = 1;
            cur->gpgcheck = 1;              /* 默认要验签 —— 而本实现验不了 */
            snprintf(cur->id, sizeof cur->id, "%.*s", (int)(e - s - 1), s + 1);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        if (!cur) {
            /* 键写在 [仓库段] 之前会被整个丢掉, 表现成"一个仓库都没有"却
             * 没有任何提示 —— 这种静默最费时间, 直接点名。 */
            fprintf(stderr, "yum: %s 里 '%s' 写在任何 [仓库名] 段之前, 忽略\n",
                    pc_basename(fp), s);
            continue;
        }
        *eq++ = 0;
        char *k = s, *v = eq;
        rstrip(k);
        while (*v == ' ' || *v == '\t')
            v++;
        if (!strcmp(k, "name"))
            snprintf(cur->name, sizeof cur->name, "%s", v);
        else if (!strcmp(k, "baseurl"))
            expand_vars(v, cur->base, sizeof cur->base);
        else if (!strcmp(k, "enabled"))
            cur->enabled = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "gpgcheck"))
            cur->gpgcheck = strcmp(v, "0") ? 1 : 0;
    }
    fclose(f);
    return 0;
}

static int cmpstrp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void read_repos(void)
{
    char d[600];
    p_reposd(d, sizeof d);
    nrepo = 0;
    DIR *dp = opendir(d);
    if (!dp)
        return;
    /* readdir 顺序不定: 先把 *.repo 收下来排序, 同一台机器上每次跑出来的
     * 仓库编号与"候选来自哪个源"才一致 */
    static char *names[128];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(dp)) && nn < 128) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 5, ".repo"))
            continue;
        names[nn++] = strdup(e->d_name);
    }
    closedir(dp);
    qsort(names, (size_t)nn, sizeof *names, cmpstrp);
    for (int i = 0; i < nn; i++) {
        char fp[700];
        snprintf(fp, sizeof fp, "%s/%s", d, names[i]);
        repo_parse_file(fp);
        free(names[i]);
        names[i] = NULL;
    }
    for (int i = 0; i < nrepo; i++) {
        if (opt_disablerepo[0] && !strcmp(repos[i].id, opt_disablerepo))
            repos[i].enabled = 0;
        if (opt_enablerepo[0] && !strcmp(repos[i].id, opt_enablerepo))
            repos[i].enabled = 1;
    }
}

/* ------------------------- 下载 ------------------------- */
static int download(const char *url, const char *dest)
{
    struct http_options o;
    struct http_result r;
    memset(&o, 0, sizeof o);
    memset(&r, 0, sizeof r);
    o.url = url;
    o.output = dest;
    o.timeout = 600;
    o.follow = 1;
    o.insecure = 0;                          /* 严格证书校验 */
    o.cacert = "/etc/ssl/cert.pem";
    o.fail_http = 1;
    int rc = http_download(&o, &r, NULL);
    if (rc != 0)
        fprintf(stderr, "yum: 下载 %s 失败: %s (code=%d)%s\n", url, r.error, rc,
                rc == 60 ? " (HTTPS 证书校验失败)" : "");
    return rc;
}

static int hex64(const char *s)
{
    if (!s || strlen(s) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

/* ------------------------- 索引结构 ------------------------- */
struct ydep {
    char name[128];
    char ver[32];
    char rel[20];
    char flags[6];
};

struct ypkg {
    char name[96];
    char arch[24];
    char epoch[8];
    char ver[32];
    char rel[24];
    char summary[128];
    char descr[256];
    char href[256];
    char ck[80];
    char cktype[12];
    long pkgsize;
    struct ydep req[MAX_REQ];
    int nreq;
    struct ydep prv[MAX_PRV];
    int nprv;
    char files[MAX_FILE][128];
    int nfile;
    int repo;
};
static struct ypkg pkgs[MAX_PKG];
static int npkg;

static void repo_cache_path(char *out, size_t n, const char *id,
                            const char *leaf)
{
    char c[600];
    p_cache(c, sizeof c);
    char d[700];
    snprintf(d, sizeof d, "%s/%s", c, id);
    pc_mkdirs(d, 0755);
    snprintf(out, n, "%s/%s", d, leaf);
}

/* 从一个 <name ...> 起始处取属性值 */
static void xml_attr(const char *e, const char *key, char *out, size_t n)
{
    if (out && n)
        out[0] = 0;
    char pat[48];
    snprintf(pat, sizeof pat, "%s=\"", key);
    const char *gt = strchr(e, '>');
    if (!gt)
        return;
    for (const char *p = e; (p = strstr(p, pat)) && p < gt;) {
        char b = p[-1];
        if (b == ' ' || b == '\t' || b == '\n') {
            p += strlen(pat);
            const char *q = strchr(p, '"');
            if (!q || q > gt)
                return;
            size_t l = (size_t)(q - p);
            if (l >= n)
                l = n - 1;
            if (out && n) {
                memcpy(out, p, l);
                out[l] = 0;
            }
            return;
        }
        p += strlen(pat) - 1;
    }
}

/* 取 <tag ...>文本</tag> 的内容(限 [start,end) 区间内) */
static void xml_text(const char *start, const char *end, const char *tag,
                     char *out, size_t n)
{
    if (out && n)
        out[0] = 0;
    char op[64], cl[72];
    snprintf(op, sizeof op, "<%s", tag);
    snprintf(cl, sizeof cl, "</%s>", tag);
    const char *p = start;
    while (p && p < end) {
        const char *e = NULL;
        for (const char *q = p; q + 4 < end; q++) {
            if (!memcmp(q, op, strlen(op))) {
                char c = q[strlen(op)];
                if (c == '>' || c == ' ' || c == '/' || c == '\t') {
                    e = q;
                    break;
                }
            }
        }
        if (!e)
            return;
        const char *gt = memchr(e, '>', (size_t)(end - e));
        if (!gt)
            return;
        if (gt[-1] == '/') {                 /* 自闭合: 没有文本 */
            p = gt + 1;
            continue;
        }
        const char *t = NULL;
        for (const char *q = gt + 1; q + 5 < end; q++) {
            if (!memcmp(q, cl, strlen(cl))) {
                t = q;
                break;
            }
        }
        if (!t)
            return;
        size_t l = (size_t)(t - (gt + 1));
        if (l >= n)
            l = n - 1;
        if (out && n) {
            memcpy(out, gt + 1, l);
            out[l] = 0;
            rstrip(out);
            char *amp;
            while ((amp = strstr(out, "&amp;"))) {
                memmove(amp + 4, amp + 5, strlen(amp + 5) + 1);
                amp[1] = 'a'; amp[2] = 0;
                memmove(amp + 1, amp + 4, strlen(amp + 4) + 1);
                *amp = '&';
            }
            while ((amp = strstr(out, "&gt;"))) {
                memmove(amp + 1, amp + 4, strlen(amp + 4) + 1);
                *amp = '>';
            }
            while ((amp = strstr(out, "&lt;"))) {
                memmove(amp + 1, amp + 4, strlen(amp + 4) + 1);
                *amp = '<';
            }
            while ((amp = strstr(out, "&quot;"))) {
                memmove(amp + 1, amp + 6, strlen(amp + 6) + 1);
                *amp = '"';
            }
        }
        return;
    }
}

static void parse_dep_entry(const char *q, struct ydep *d)
{
    memset(d, 0, sizeof *d);
    xml_attr(q, "name", d->name, sizeof d->name);
    xml_attr(q, "ver", d->ver, sizeof d->ver);
    xml_attr(q, "rel", d->rel, sizeof d->rel);
    xml_attr(q, "flags", d->flags, sizeof d->flags);
}

/* 解析一段 primary.xml */
static void parse_primary(const char *xml, size_t xlen, int ri)
{
    const char *end = xml + xlen;
    const char *p = xml;
    while ((p = strstr(p, "<package ")) && p < end) {
        const char *close = strstr(p, "</package>");
        if (!close || close > end)
            break;
        const char *bend = close + 10;
        if (npkg >= MAX_PKG) {
            fprintf(stderr, "yum: 包数超过上限 %d, 后面的忽略\n", MAX_PKG);
            break;
        }
        struct ypkg *k = &pkgs[npkg];
        memset(k, 0, sizeof *k);
        k->repo = ri;
        xml_text(p, bend, "name", k->name, sizeof k->name);
        xml_text(p, bend, "arch", k->arch, sizeof k->arch);
        xml_text(p, bend, "summary", k->summary, sizeof k->summary);
        xml_text(p, bend, "description", k->descr, sizeof k->descr);
        xml_text(p, bend, "checksum", k->ck, sizeof k->ck);
        const char *ve = strstr(p, "<version ");
        if (ve && ve < bend) {
            xml_attr(ve, "epoch", k->epoch, sizeof k->epoch);
            xml_attr(ve, "ver", k->ver, sizeof k->ver);
            xml_attr(ve, "rel", k->rel, sizeof k->rel);
        }
        const char *ce = strstr(p, "<checksum ");
        if (ce && ce < bend)
            xml_attr(ce, "type", k->cktype, sizeof k->cktype);
        const char *le = strstr(p, "<location ");
        if (le && le < bend)
            xml_attr(le, "href", k->href, sizeof k->href);
        const char *se = strstr(p, "<size ");
        if (se && se < bend) {
            char n[24];
            xml_attr(se, "package", n, sizeof n);
            k->pkgsize = strtol(n, NULL, 10);
        }
        const char *rq = strstr(p, "<rpm:requires>");
        if (rq && rq < bend) {
            const char *re = strstr(rq, "</rpm:requires>");
            if (re && re < bend) {
                for (const char *q = rq; (q = strstr(q, "<rpm:entry")) && q < re;) {
                    if (k->nreq >= MAX_REQ)
                        break;
                    parse_dep_entry(q, &k->req[k->nreq++]);
                    q += 10;
                }
            }
        }
        const char *pq = strstr(p, "<rpm:provides>");
        if (pq && pq < bend) {
            const char *pe = strstr(pq, "</rpm:provides>");
            if (pe && pe < bend) {
                for (const char *q = pq; (q = strstr(q, "<rpm:entry")) && q < pe;) {
                    if (k->nprv >= MAX_PRV)
                        break;
                    parse_dep_entry(q, &k->prv[k->nprv++]);
                    q += 10;
                }
            }
        }
        for (const char *q = p; (q = strstr(q, "<file>")) && q < bend;) {
            const char *qe = strstr(q, "</file>");
            if (!qe || qe >= bend)
                break;
            if (k->nfile < MAX_FILE) {
                size_t l = (size_t)(qe - (q + 6));
                if (l >= sizeof k->files[0])
                    l = sizeof k->files[0] - 1;
                memcpy(k->files[k->nfile], q + 6, l);
                k->files[k->nfile][l] = 0;
                k->nfile++;
            }
            q = qe + 7;
        }
        if (k->name[0])
            npkg++;
        p = bend;
    }
}

static int load_cached(int ri)
{
    char fp[760];
    repo_cache_path(fp, sizeof fp, repos[ri].id, "primary.xml");
    long len;
    unsigned char *b = pc_slurp(fp, &len);
    if (!b)
        return -1;
    parse_primary((char *)b, (size_t)len, ri);
    free(b);
    return 0;
}

static void load_all_cached(void)
{
    read_repos();
    for (int i = 0; i < nrepo; i++)
        if (repos[i].enabled)
            load_cached(i);
}

/* 拉 repomd.xml → 校验并取 primary → 解析 */
static int repo_refresh(int ri)
{
    struct repo *rp = &repos[ri];
    if (!rp->base[0]) {
        fprintf(stderr, "yum: 仓库 %s 没有 baseurl\n", rp->id);
        return -1;
    }
    char url[900], cache[760], path[760];
    snprintf(url, sizeof url, "%s/repodata/repomd.xml", rp->base);
    /* 缓存路径就是 <cache>/<仓库 id>/repomd.xml —— 别把 id 再拼进 leaf,
     * 那会写成 <cache>/<id>/<id>/repomd.xml, 换机器/换顺序就找不回来。 */
    repo_cache_path(path, sizeof path, rp->id, "repomd.xml");
    unlink(path);
    if (download(url, path) != 0)
        return -1;
    long rlen;
    unsigned char *rb = pc_slurp(path, &rlen);
    if (!rb)
        return -1;
    char href[320] = "", want[80] = "", ctype[16] = "";
    const char *p = (char *)rb;
    int found = 0;
    while (!found && (p = strstr(p, "<data type=")) && p < (char *)rb + rlen) {
        char t[32];
        xml_attr(p, "type", t, sizeof t);
        if (strcmp(t, "primary")) {
            p += 10;
            continue;
        }
        const char *de = strstr(p, "</data>");
        if (!de)
            break;
        xml_text(p, de, "checksum", want, sizeof want);
        const char *ce = strstr(p, "<checksum ");
        if (ce && ce < de)
            xml_attr(ce, "type", ctype, sizeof ctype);
        const char *le = strstr(p, "<location ");
        if (le && le < de)
            xml_attr(le, "href", href, sizeof href);
        found = 1;
    }
    free(rb);
    if (!found || !href[0]) {
        fprintf(stderr, "yum: 仓库 %s 的 repomd.xml 里没有 primary 条目\n",
                rp->id);
        return -1;
    }
    if (strcmp(ctype, "sha256")) {
        fprintf(stderr, "yum: 仓库 %s 的 primary 摘要类型是 %s —— 本实现只支持\n"
                        "     sha256, 不做\"算不了就当通过\"\n", rp->id,
                ctype[0] ? ctype : "(空)");
        return -1;
    }
    if (!hex64(want)) {
        fprintf(stderr, "yum: 仓库 %s 声明的 primary 摘要不是 64 位十六进制\n",
                rp->id);
        return -1;
    }
    char dl[760];
    repo_cache_path(dl, sizeof dl, rp->id, "primary.download");
    unlink(dl);
    snprintf(url, sizeof url, "%s/%s", rp->base, href);
    if (download(url, dl) != 0)
        return -1;
    long dlen;
    unsigned char *db = pc_slurp(dl, &dlen);
    if (!db) {
        unlink(dl);
        return -1;
    }
    char got[65];
    pc_sha256_hex(db, (size_t)dlen, got);
    if (strcasecmp(got, want)) {
        fprintf(stderr, "yum: 仓库 %s 的索引摘要不符(repomd 说 %s, 实得 %s)\n"
                        "     —— 索引被改过或下载不完整, 这个仓库不用\n",
                rp->id, want, got);
        free(db);
        unlink(dl);
        return -1;
    }
    unsigned char *xb = NULL;
    long xlen = 0;
    int owned = 0;
    if (pc_maybe_gunzip(db, dlen, &xb, &xlen, &owned) != 0) {
        fprintf(stderr, "yum: 仓库 %s 的 primary 解压失败(%s)\n", rp->id, href);
        free(db);
        unlink(dl);
        return -1;
    }
    if (xlen > XML_MAX_BYTES) {
        fprintf(stderr, "yum: primary.xml %ld 字节, 超过上限 %ld\n",
                xlen, XML_MAX_BYTES);
        if (owned)
            free(xb);
        free(db);
        unlink(dl);
        return -1;
    }
    int before = npkg;
    parse_primary((char *)xb, (size_t)xlen, ri);
    repo_cache_path(cache, sizeof cache, rp->id, "primary.xml");
    unlink(cache);
    if (pc_write_file(cache, xb, xlen, 0644) != 0) {
        fprintf(stderr, "yum: 写索引缓存 %s 失败\n", cache);
        if (owned)
            free(xb);
        free(db);
        unlink(dl);
        return -1;
    }
    printf("仓库 %s(%s): 读到 %d 个包\n", rp->id, rp->base, npkg - before);
    if (owned)
        free(xb);
    free(db);
    unlink(dl);
    return 0;
}

/* ------------------------- 已装库(读自研 rpm 的状态) ------------------------- */
struct iv {
    char name[96];
    char nv[64];                          /* version-release */
};
static struct iv ivs[512];
static int niv;

static void load_installed(void)
{
    char d[640];
    p_rpmdb(d, sizeof d);
    DIR *dp = opendir(d);
    niv = 0;
    if (!dp)
        return;
    struct dirent *e;
    while ((e = readdir(dp)) && niv < 512) {
        char *dot = strstr(e->d_name, ".meta");
        if (!dot)
            continue;
        char fp[760];
        snprintf(fp, sizeof fp, "%s/%s", d, e->d_name);
        long len;
        unsigned char *b = pc_slurp(fp, &len);
        if (!b)
            continue;
        char name[96] = "", ver[64] = "", rel[64] = "";
        char *line = (char *)b;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = 0;
            char *eq = strchr(line, '=');
            if (eq) {
                *eq++ = 0;
                if (!strcmp(line, "name"))
                    snprintf(name, sizeof name, "%s", eq);
                else if (!strcmp(line, "version"))
                    snprintf(ver, sizeof ver, "%s", eq);
                else if (!strcmp(line, "release"))
                    snprintf(rel, sizeof rel, "%s", eq);
            }
            if (!nl)
                break;
            line = nl + 1;
        }
        free(b);
        if (!name[0])
            continue;
        snprintf(ivs[niv].name, sizeof ivs[niv].name, "%s", name);
        snprintf(ivs[niv].nv, sizeof ivs[niv].nv, "%s-%s", ver, rel);
        niv++;
    }
    closedir(dp);
}

static int installed_matches(const char *name, const char *flags,
                             const char *ver, const char *rel)
{
    char want[80];
    snprintf(want, sizeof want, "%s-%s", ver, rel);
    for (int i = 0; i < niv; i++) {
        if (strcmp(ivs[i].name, name))
            continue;
        if (!flags[0] || !ver[0])
            return 1;
        if (pc_rpm_ver_match(ivs[i].nv, flags, want))
            return 1;
    }
    return 0;
}

static int file_present(const char *p)
{
    char full[1300];
    snprintf(full, sizeof full, "%s%s", opt_root, p);
    struct stat st;
    return lstat(full, &st) == 0;
}

/* ------------------------- 候选与依赖 ------------------------- */
static struct ypkg *best_candidate(const char *name)
{
    struct ypkg *best = NULL;
    for (int i = 0; i < npkg; i++) {
        struct ypkg *k = &pkgs[i];
        int hit = !strcmp(k->name, name);
        if (!hit) {
            char t[300];
            snprintf(t, sizeof t, "%s-%s-%s", k->name, k->ver, k->rel);
            if (!strcmp(t, name))
                hit = 1;
            if (!hit) {
                snprintf(t, sizeof t, "%s-%s-%s.%s", k->name, k->ver, k->rel,
                         k->arch);
                if (!strcmp(t, name))
                    hit = 1;
            }
        }
        if (!hit)
            continue;
        if (k->arch[0] && strcmp(k->arch, "noarch") &&
            strcmp(k->arch, host_basearch()))
            continue;
        if (!best)
            best = k;
        else {
            char a[80], b[80];
            snprintf(a, sizeof a, "%s-%s", k->ver, k->rel);
            snprintf(b, sizeof b, "%s-%s", best->ver, best->rel);
            if (pc_rpm_vercmp(a, b) > 0)
                best = k;
        }
    }
    return best;
}

static int pkg_provides(struct ypkg *k, const struct ydep *d)
{
    char have[80], want[80];
    snprintf(want, sizeof want, "%s-%s", d->ver, d->rel);
    for (int i = 0; i < k->nprv; i++) {
        if (strcmp(k->prv[i].name, d->name))
            continue;
        if (!d->flags[0] || !d->ver[0])
            return 1;
        snprintf(have, sizeof have, "%s-%s", k->prv[i].ver, k->prv[i].rel);
        if (pc_rpm_ver_match(have, d->flags, want))
            return 1;
    }
    if (!strcmp(k->name, d->name)) {
        if (!d->flags[0] || !d->ver[0])
            return 1;
        snprintf(have, sizeof have, "%s-%s", k->ver, k->rel);
        if (pc_rpm_ver_match(have, d->flags, want))
            return 1;
    }
    return 0;
}

static int pkg_provides_file(struct ypkg *k, const char *p)
{
    for (int i = 0; i < k->nfile; i++)
        if (!strcmp(k->files[i], p))
            return 1;
    return 0;
}

static int req_satisfied(struct ydep *d, struct ypkg **sel, int n)
{
    if (!strncmp(d->name, "rpmlib(", 7))
        return 1;                            /* 包管理器自带能力, 上游也这样 */
    if (d->name[0] == '/') {
        if (file_present(d->name))
            return 1;
        for (int i = 0; i < n; i++)
            if (pkg_provides_file(sel[i], d->name))
                return 1;
        return 0;
    }
    if (installed_matches(d->name, d->flags, d->ver, d->rel))
        return 1;
    for (int i = 0; i < n; i++)
        if (pkg_provides(sel[i], d))
            return 1;
    return 0;
}

struct res {
    struct ypkg *sel[MAX_PKG];
    int n;
};

/* 深度优先: 被依赖者排在前面(rpm 脚本段的执行顺序要这样) */
static int visit(struct res *r, struct ypkg *k, int depth)
{
    if (depth > 48) {
        fprintf(stderr, "yum: 依赖递归超过 48 层(循环依赖?)\n");
        return -1;
    }
    for (int i = 0; i < r->n; i++)
        if (r->sel[i] == k)
            return 0;
    if (r->n >= MAX_PKG)
        return -1;
    r->sel[r->n++] = k;
    for (int d = 0; d < k->nreq; d++) {
        struct ydep *dep = &k->req[d];
        if (req_satisfied(dep, r->sel, r->n))
            continue;
        struct ypkg *p = best_candidate(dep->name);
        if (!p) {
            for (int i = 0; i < npkg && !p; i++) {
                if (dep->name[0] == '/' &&
                    pkg_provides_file(&pkgs[i], dep->name))
                    p = &pkgs[i];
                else if (dep->name[0] != '/' && pkg_provides(&pkgs[i], dep))
                    p = &pkgs[i];
            }
        }
        if (!p) {
            fprintf(stderr, "yum: 没有任何包提供 %s(%s 需要它)\n", dep->name,
                    k->name);
            return -1;
        }
        if (p != k && visit(r, p, depth + 1) < 0)
            return -1;
    }
    return 0;
}

/* ------------------------- 调 rpm ------------------------- */
static int run_rpm(char **av)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "yum: fork 失败: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(av[0], av);
        execvp(av[0], av);
        fprintf(stderr, "yum: 跑不了 %s —— 装包必须靠它: %s\n",
                av[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

static int rpm_do(const char *flag, char **args, int n)
{
    char *av[80];
    int i = 0;
    av[i++] = rpm_path;
    if (opt_root[0]) {
        av[i++] = "--root";
        av[i++] = opt_root;
    }
    av[i++] = (char *)flag;
    for (int j = 0; j < n && i < 76; j++)
        av[i++] = args[j];
    av[i] = NULL;
    return run_rpm(av);
}

static int confirm(const char *what)
{
    if (opt_yes)
        return 1;
    if (!isatty(0)) {
        fprintf(stderr, "yum: 非交互终端要%s请显式加 -y(--assumeyes)\n", what);
        return 0;
    }
    printf("要继续吗? [Y/n] ");
    fflush(stdout);
    char line[128];
    if (!fgets(line, sizeof line, stdin))
        return 0;
    return (line[0] == 'y' || line[0] == 'Y' || line[0] == '\n');
}

/* ------------------------- 动作 ------------------------- */
static int do_makecache(void)
{
    read_repos();
    if (!nrepo) {
        char d[600];
        p_reposd(d, sizeof d);
        fprintf(stderr, "yum: %s 里没有可用的仓库(要么没有 .repo 文件,"
                        " 要么键都写在 [仓库名] 段之前)\n", d);
        return 1;
    }
    int ok = 0, bad = 0;
    for (int i = 0; i < nrepo; i++) {
        if (!repos[i].enabled)
            continue;
        if (repo_refresh(i) == 0)
            ok++;
        else
            bad++;
    }
    if (!ok) {
        fprintf(stderr, "yum: %d 个启用的仓库全部失败\n", bad);
        return 1;
    }
    printf("yum: %d 个仓库已缓存, %d 个失败\n", ok, bad);
    return ok ? 0 : 1;
}

static int do_install(char **want, int nwant, int upgrade)
{
    read_repos();
    load_installed();
    load_all_cached();
    if (!npkg) {
        fprintf(stderr, "yum: 索引是空的 —— 先跑 yum makecache\n");
        return 1;
    }
    struct res r;
    memset(&r, 0, sizeof r);
    int skipped = 0;
    for (int i = 0; i < nwant; i++) {
        struct ypkg *k = best_candidate(want[i]);
        if (!k) {
            fprintf(stderr, "yum: 仓库里没有 %s(先 yum makecache?)\n", want[i]);
            return 1;
        }
        int mark = r.n;
        if (visit(&r, k, 0) < 0) {
            if (!opt_skip_broken) {
                fprintf(stderr, "yum: 依赖无法满足, 已中止(什么都没装)\n");
                return 1;
            }
            /* --skip-broken 的语义是"丢掉这个请求", 不是"没依赖也照装" ——
             * 后者会装出一个跑不起来的系统, 比不装更坏。 */
            r.n = mark;
            skipped = 1;
            fprintf(stderr, "yum: 跳过 %s(--skip-broken)\n", k->name);
            continue;
        }
    }
    if (!r.n) {
        if (skipped) {
            /* --skip-broken 的语义就是"这种请求丢掉不算错": 全被丢掉时
             * 退出 0, 但一行说清楚什么都没装。 */
            printf("yum: 没有可装的包(全部因依赖不满足被跳过, 什么都没装)\n");
            return 0;
        }
        printf("yum: 没有什么要装的\n");
        return 1;
    }
    int n = r.n;
    printf("将要%s %d 个包:\n", upgrade ? "升级/安装" : "安装", n);
    for (int i = 0; i < n; i++)
        printf("  %s.%s %s-%s\n", r.sel[i]->name, r.sel[i]->arch,
               r.sel[i]->ver, r.sel[i]->rel);
    if (!confirm("安装"))
        return 1;
    if (!opt_nogpg) {
        for (int i = 0; i < n; i++) {
            struct repo *rp = &repos[r.sel[i]->repo];
            if (rp->gpgcheck) {
                fprintf(stderr,
                        "yum: 拒绝安装 %s: 仓库 %s 里 gpgcheck=1(或没写),"
                        " 而本实现**不验包签名**。\n"
                        "     要么在那份 .repo 里显式写 gpgcheck=0,"
                        " 要么加 --nogpgcheck。\n",
                        r.sel[i]->name, rp->id);
                return 1;
            }
        }
    }
    char **files = calloc((size_t)(n > 0 ? n : 1), sizeof *files);
    if (!files)
        return 1;
    int nf = 0, fail = 0;
    for (int i = 0; i < n && !fail; i++) {
        struct ypkg *k = r.sel[i];
        if (!k->href[0]) {
            fprintf(stderr, "yum: %s 的索引里没有 location href\n", k->name);
            fail = 1;
            break;
        }
        if (strcmp(k->cktype, "sha256")) {
            fprintf(stderr, "yum: %s 的包体摘要类型是 %s, 只支持 sha256\n",
                    k->name, k->cktype[0] ? k->cktype : "(空)");
            fail = 1;
            break;
        }
        char url[900], dest[800];
        snprintf(url, sizeof url, "%s/%s", repos[k->repo].base, k->href);
        char leaf[300];
        snprintf(leaf, sizeof leaf, "%s", pc_basename(k->href));
        /* 下载缓存: <cache>/<仓库 id>/<文件名> —— 同名不同仓库不打架 */
        repo_cache_path(dest, sizeof dest, repos[k->repo].id, leaf);
        struct stat st;
        int reuse = 0;
        if (stat(dest, &st) == 0 && hex64(k->ck)) {
            char hex[65];
            if (pc_file_sha256_hex(dest, hex) == 0 && !strcasecmp(hex, k->ck))
                reuse = 1;
        }
        if (!reuse) {
            printf("获取 %s\n", url);
            if (download(url, dest) != 0) {
                unlink(dest);
                fail = 1;
                break;
            }
        }
        if (stat(dest, &st) != 0) {
            fprintf(stderr, "yum: %s 下载后不见了\n", dest);
            fail = 1;
            break;
        }
        if (k->pkgsize && (long)st.st_size != k->pkgsize) {
            fprintf(stderr, "yum: %s 大小不符(索引 %ld, 实得 %ld) ——"
                            " 删掉缓存, 不交给 rpm\n", pc_basename(k->href),
                    k->pkgsize, (long)st.st_size);
            unlink(dest);
            fail = 1;
            break;
        }
        if (hex64(k->ck)) {
            char hex[65];
            if (pc_file_sha256_hex(dest, hex) != 0) {
                fprintf(stderr, "yum: 算不出 %s 的 sha256\n", dest);
                fail = 1;
                break;
            }
            if (strcasecmp(hex, k->ck)) {
                fprintf(stderr, "yum: %s 的 sha256 与索引不符(索引 %s,"
                                " 实得 %s) —— 删掉缓存, 不交给 rpm\n",
                        pc_basename(k->href), k->ck, hex);
                unlink(dest);
                fail = 1;
                break;
            }
        }
        files[nf++] = strdup(dest);
    }
    if (fail) {
        for (int i = 0; i < nf; i++)
            free(files[i]);
        free(files);
        return 1;
    }
    printf("正在交给 rpm %s...\n", upgrade ? "-U" : "-i");
    int rr = rpm_do(upgrade ? "-U" : "-i", files, nf);
    for (int i = 0; i < nf; i++)
        free(files[i]);
    free(files);
    if (rr != 0) {
        fprintf(stderr, "yum: rpm 返回 %d(它的错误在上面)\n", rr);
        return 1;
    }
    printf("已装 %d 个包\n", nf);
    return 0;
}

static int do_remove(char **names, int n)
{
    load_installed();
    if (!n) {
        fprintf(stderr, "yum: remove 需要包名\n");
        return 1;
    }
    for (int i = 0; i < n; i++) {
        int found = 0;
        for (int j = 0; j < niv; j++)
            if (!strcmp(ivs[j].name, names[i]))
                found = 1;
        if (!found) {
            fprintf(stderr, "yum: 包 %s 没装过(rpm 数据库里没有)\n", names[i]);
            return 1;
        }
    }
    if (!confirm("卸载"))
        return 1;
    int rr = rpm_do("-e", names, n);
    if (rr == 0)
        printf("yum: 完成\n");
    return rr ? 1 : 0;
}

static int do_list(char **pat, int np, int installed_only)
{
    load_installed();
    load_all_cached();
    if (installed_only) {
        printf("已装包(rpm 数据库):\n");
        if (!niv)
            printf("  (无)\n");
        for (int i = 0; i < niv; i++) {
            if (np && !strcasestr(ivs[i].name, pat[0]))
                continue;
            printf("  %s %s\n", ivs[i].name, ivs[i].nv);
        }
        return 0;
    }
    printf("可装包(索引):\n");
    int any = 0;
    for (int i = 0; i < npkg; i++) {
        struct ypkg *k = &pkgs[i];
        if (np && !strcasestr(k->name, pat[0]))
            continue;
        printf("  %s.%s %s-%s %s\n", k->name, k->arch, k->ver, k->rel,
               repos[k->repo].id);
        any = 1;
    }
    if (!any)
        printf("  (没有匹配的包, 或索引为空 —— 先 yum makecache)\n");
    return 0;
}

static int do_info(char **names, int n)
{
    load_installed();
    load_all_cached();
    int rc = 0;
    for (int i = 0; i < n; i++) {
        struct ypkg *k = best_candidate(names[i]);
        if (!k) {
            fprintf(stderr, "yum: 仓库里没有 %s\n", names[i]);
            rc = 1;
            continue;
        }
        printf("Name    : %s\nArch    : %s\nVersion : %s\nRelease : %s\n"
               "Size    : %ld\nRepo    : %s(gpgcheck=%d)\nSummary : %s\n",
               k->name, k->arch, k->ver, k->rel, k->pkgsize,
               repos[k->repo].id, repos[k->repo].gpgcheck, k->summary);
        if (k->descr[0])
            printf("Desc    : %s\n", k->descr);
        if (k->nreq) {
            printf("Requires:");
            for (int j = 0; j < k->nreq; j++)
                printf(" %s%s%s%s%s", k->req[j].name, k->req[j].flags[0] ? " " : "",
                       k->req[j].flags, k->req[j].ver, "");
            printf("\n");
        }
        printf("\n");
    }
    return rc;
}

static int do_search(char **kw, int n)
{
    load_all_cached();
    int hits = 0;
    for (int i = 0; i < npkg; i++) {
        for (int j = 0; j < n; j++) {
            if (strcasestr(pkgs[i].name, kw[j]) ||
                strcasestr(pkgs[i].summary, kw[j]) ||
                strcasestr(pkgs[i].descr, kw[j])) {
                printf("%s.%s %s-%s : %s\n", pkgs[i].name, pkgs[i].arch,
                       pkgs[i].ver, pkgs[i].rel, pkgs[i].summary);
                hits++;
                break;
            }
        }
    }
    if (!hits)
        printf("yum: 没有匹配的包\n");
    return 0;
}

static int do_repolist(void)
{
    read_repos();
    printf("%-14s %-34s %-8s %s\n", "repo id", "baseurl", "gpgcheck", "启用");
    for (int i = 0; i < nrepo; i++)
        printf("%-14s %-34s %-8d %s\n", repos[i].id, repos[i].base,
               repos[i].gpgcheck, repos[i].enabled ? "是" : "否");
    return 0;
}

static int do_clean(void)
{
    char c[600];
    p_cache(c, sizeof c);
    DIR *d = opendir(c);
    if (!d) {
        printf("yum: 没有缓存目录\n");
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char sub[700];
        snprintf(sub, sizeof sub, "%s/%s", c, e->d_name);
        DIR *sd = opendir(sub);
        if (!sd) {
            if (unlink(sub) == 0)
                n++;
            continue;
        }
        struct dirent *f;
        while ((f = readdir(sd))) {
            if (f->d_name[0] == '.')
                continue;
            char fp[800];
            snprintf(fp, sizeof fp, "%s/%s", sub, f->d_name);
            if (unlink(fp) == 0)
                n++;
        }
        closedir(sd);
        rmdir(sub);
    }
    closedir(d);
    printf("yum: 清掉 %d 个缓存文件\n", n);
    return 0;
}

static void usage(void)
{
    printf("yum %s (Parlz 移植实现; 解包安装由 rpm 完成)\n", YUM_VERSION);
    printf("用法: yum [选项] 动作 [参数]\n");
    printf("  makecache                 拉 repodata(primary.xml)并缓存\n");
    printf("  install <包>...            求解依赖 → 下载 → 校验 → rpm -i\n");
    printf("  update [包]...             带包名时 rpm -U, 否则等于 makecache\n");
    printf("  remove|erase <包>...        rpm -e\n");
    printf("  list [installed|available] [模式]\n");
    printf("  info <包>...   search <关键字>...   repolist   clean\n");
    printf("选项: --installroot <目录> --rpm <路径> -y|--assumeyes\n");
    printf("      --nogpgcheck --skip-broken --releasever=<值>\n");
    printf("      --disablerepo=<id> --enablerepo=<id> --version\n");
    printf("仓库: /etc/yum.repos.d/*.repo —— [id] baseurl= enabled= gpgcheck=\n");
}

int main(int argc, char **argv)
{
    const char *act = NULL;
    char *args[64];
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--version")) {
            printf("yum %s\n", YUM_VERSION);
            return 0;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(a, "--installroot") || !strcmp(a, "--rpm")) {
            int isr = !strcmp(a, "--installroot");
            if (++i >= argc) {
                fprintf(stderr, "yum: %s 需要参数\n", a);
                return 1;
            }
            char *dst = isr ? opt_root : rpm_path;
            size_t dn = isr ? sizeof opt_root : sizeof rpm_path;
            snprintf(dst, dn, "%s", argv[i]);
            if (isr) {
                char *sl = dst + strlen(dst) - 1;
                while (sl > dst && *sl == '/')
                    *sl-- = 0;
            }
            continue;
        }
        if (!strncmp(a, "--releasever=", 13)) {
            snprintf(opt_releasever, sizeof opt_releasever, "%s", a + 13);
            continue;
        }
        if (!strncmp(a, "--disablerepo=", 14)) {
            snprintf(opt_disablerepo, sizeof opt_disablerepo, "%s", a + 14);
            continue;
        }
        if (!strncmp(a, "--enablerepo=", 13)) {
            snprintf(opt_enablerepo, sizeof opt_enablerepo, "%s", a + 13);
            continue;
        }
        if (!strcmp(a, "-y") || !strcmp(a, "--assumeyes") ||
            !strcmp(a, "--yes")) { opt_yes = 1; continue; }
        if (!strcmp(a, "--nogpgcheck")) { opt_nogpg = 1; continue; }
        if (!strcmp(a, "--skip-broken")) { opt_skip_broken = 1; continue; }
        if (!strncmp(a, "--setopt=", 9) || !strcmp(a, "-q") ||
            !strcmp(a, "--quiet") || !strcmp(a, "--verbose") ||
            !strcmp(a, "-C") || !strcmp(a, "--cacheonly"))
            continue;                         /* 上游旗标, 本实现里是空操作 */
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "yum: 不认的选项 %s(见 yum --help)\n", a);
            return 1;
        }
        if (!act) {
            act = a;
            continue;
        }
        if (nargs < 64)
            args[nargs++] = a;
    }
    if (!act) {
        usage();
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(act, "makecache") || !strcmp(act, "update-cache"))
        return do_makecache();
    if (!strcmp(act, "install")) {
        if (!nargs) {
            fprintf(stderr, "yum: install 需要包名\n");
            return 1;
        }
        return do_install(args, nargs, 0);
    }
    if (!strcmp(act, "update")) {
        if (!nargs)
            return do_makecache();
        return do_install(args, nargs, 1);
    }
    if (!strcmp(act, "remove") || !strcmp(act, "erase"))
        return do_remove(args, nargs);
    if (!strcmp(act, "list")) {
        int installed_only = 0;
        int off = 0;
        if (nargs && !strcmp(args[0], "installed")) {
            installed_only = 1;
            off = 1;
        } else if (nargs && !strcmp(args[0], "available"))
            off = 1;
        return do_list(args + off, nargs - off, installed_only);
    }
    if (!strcmp(act, "info"))
        return do_info(args, nargs);
    if (!strcmp(act, "search"))
        return do_search(args, nargs);
    if (!strcmp(act, "repolist"))
        return do_repolist();
    if (!strcmp(act, "clean")) {
        if (nargs && strcmp(args[0], "all"))
            fprintf(stderr, "yum: clean 只实现了 all\n");
        return do_clean();
    }
    fprintf(stderr, "yum: 未实现的动作 %s\n", act);
    return 1;
}
