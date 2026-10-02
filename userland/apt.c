// SPDX-License-Identifier: GPL-2.0-or-later
/* apt.c - Parlz 移植的 APT 前端(高层命令, 落盘交给 dpkg)。
 *
 * 分工与 Debian 一致: apt 只管"源 → 索引 → 依赖求解 → 下载", 真正解包安装
 * 一律 fork+execv 调 /bin/dpkg(本仓库的移植实现)。
 *
 * 源: /etc/apt/sources.list, 行格式
 *     deb [选项] <URI> <发行> <组件>...
 *   选项支持 trusted=yes/no 与 arch=<架构>(可逗号分隔多个; 默认本机架构 + all)。
 * 索引: <URI>/dists/<发行>/<组件>/binary-<架构>/Packages[.gz]
 *   缓存到 /var/lib/apt/lists/<源序号>_<组件序号>_<架构>_Packages(解压后的原文),
 *   段落空行分隔, 字段与 Debian 一致(Package/Version/Architecture/Depends/
 *   Pre-Depends/Provides/Filename/Size/SHA256/Description...)。
 *
 * 动作:
 *   apt update              拉所有源的索引
 *   apt install <包>...      求解依赖 → 下载 → 校验 → dpkg -i
 *   apt remove|purge <包>...  卸载(转交 dpkg, 带反向依赖保护)
 *   apt search <关键字>...    在名字与描述里找
 *   apt show <包>...         打印索引记录
 *   apt list [--installed] [模式]
 *   apt policy [包]          已装版本 / 候选版本 / 来自哪个源
 *   apt clean                清下载缓存
 * 选项:
 *   --root <目录>(所有路径含 dpkg 的 root 一起挪)  -y|--yes
 *   --allow-unauthenticated   --print-uris   --no-download
 *   --dpkg <路径>   --version
 *
 * 安全边界(说清楚, 不冒充完整的 apt):
 *   - **不验 GPG/InRelease 签名**。没标 trusted=yes 的源里的包一律拒绝安装,
 *     除非显式 --allow-unauthenticated —— 这是本实现里替代签名校验的闸门。
 *   - 下载后按索引声明的 Size + SHA256 校验; 不符就删掉缓存并失败,
 *     绝不把内容不对的档交给 dpkg。
 *   - 依赖只解 Depends/Pre-Depends(含 "a | b" 或关系与版本约束)与
 *     已装包的 Provides; 不处理 Recommends/Suggests/Conflicts/Breaks,
 *     也没有 apt 的事务回滚。
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

#define APT_VERSION "1.0.0-parlz"
#define MAX_SRC 16
#define MAX_COMP 8
#define MAX_ARCH 4
#define MAX_PKG 8000
#define MAX_DEPS 32
#define MAX_ALT 4
#define IDX_MAX_BYTES (64L * 1024 * 1024)

static char opt_root[512];            /* "" = 真实根 */
static int opt_yes, opt_allow_untrusted, opt_print_uris, opt_no_download;
static char dpkg_path[256] = "/bin/dpkg";

/* ------------------------- 路径 ------------------------- */
static void p_lists(char *out, size_t n)
{
    snprintf(out, n, "%s/var/lib/apt/lists", opt_root);
}
static void p_archives(char *out, size_t n)
{
    snprintf(out, n, "%s/var/cache/apt/archives", opt_root);
}
static void p_sources(char *out, size_t n)
{
    snprintf(out, n, "%s/etc/apt/sources.list", opt_root);
}
static void p_status(char *out, size_t n)
{
    snprintf(out, n, "%s/var/lib/dpkg/status", opt_root);
}

static const char *host_deb_arch(void)
{
    static char a[32];
    if (a[0])
        return a;
    struct utsname u;
    if (uname(&u) == 0) {
        if (!strcmp(u.machine, "x86_64"))
            snprintf(a, sizeof a, "amd64");
        else if (!strcmp(u.machine, "aarch64"))
            snprintf(a, sizeof a, "arm64");
        else if (!strcmp(u.machine, "i686") || !strcmp(u.machine, "i386"))
            snprintf(a, sizeof a, "i386");
        else
            snprintf(a, sizeof a, "%s", u.machine);
    } else
        snprintf(a, sizeof a, "amd64");
    return a;
}

/* ------------------------- 源 ------------------------- */
struct src {
    char base[256];
    char dist[128];
    char comp[MAX_COMP][64];
    int ncomp;
    int trusted;
    char arch[MAX_ARCH][32];
    int narch;
};
static struct src srcs[MAX_SRC];
static int nsrc;

static char *xstrndup(const char *s, size_t n)
{
    char *o = malloc(n + 1);
    if (!o)
        return NULL;
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

static void src_add_arch(struct src *sp, const char *a)
{
    if (sp->narch >= MAX_ARCH)
        return;
    for (int i = 0; i < sp->narch; i++)
        if (!strcmp(sp->arch[i], a))
            return;
    snprintf(sp->arch[sp->narch++], 32, "%s", a);
}

static void read_sources(void)
{
    char p[600];
    p_sources(p, sizeof p);
    FILE *f = fopen(p, "r");
    nsrc = 0;
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f) && nsrc < MAX_SRC) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#')
            continue;
        if (strncmp(s, "deb ", 4) != 0) {
            fprintf(stderr, "apt: 跳过不是 deb 的一行: %s\n", line);
            continue;
        }
        s += 4;
        while (*s == ' ' || *s == '\t')
            s++;
        struct src *sp = &srcs[nsrc];
        memset(sp, 0, sizeof *sp);
        sp->trusted = 0;                    /* 默认不信任: 没有签名校验就别乱装 */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) {
                fprintf(stderr, "apt: 这一行的 [ 选项没闭合: %s\n", line);
                continue;
            }
            *end = 0;
            char *save = NULL;
            for (char *t = strtok_r(s + 1, " \t", &save); t;
                 t = strtok_r(NULL, " \t", &save)) {
                if (!strcmp(t, "trusted=yes"))
                    sp->trusted = 1;
                else if (!strcmp(t, "trusted=no"))
                    sp->trusted = 0;
                else if (!strncmp(t, "arch=", 5)) {
                    char *save2 = NULL;
                    for (char *a = strtok_r(t + 5, ",", &save2); a;
                         a = strtok_r(NULL, ",", &save2))
                        src_add_arch(sp, a);
                } else
                    fprintf(stderr, "apt: 忽略不认的源选项 '%s'\n", t);
            }
            s = end + 1;
            while (*s == ' ' || *s == '\t')
                s++;
        }
        if (!sp->narch) {
            src_add_arch(sp, host_deb_arch());
            src_add_arch(sp, "all");
        }
        char *save = NULL;
        char *uri = strtok_r(s, " \t", &save);
        char *dist = strtok_r(NULL, " \t", &save);
        if (!uri || !dist) {
            fprintf(stderr, "apt: deb 行缺 URI 或发行: %s\n", line);
            continue;
        }
        snprintf(sp->base, sizeof sp->base, "%s", uri);
        char *sl = sp->base + strlen(sp->base) - 1;
        while (sl > sp->base && *sl == '/')
            *sl-- = 0;
        snprintf(sp->dist, sizeof sp->dist, "%s", dist);
        for (char *c = strtok_r(NULL, " \t", &save); c && sp->ncomp < MAX_COMP;
             c = strtok_r(NULL, " \t", &save))
            snprintf(sp->comp[sp->ncomp++], 64, "%s", c);
        if (!sp->ncomp) {
            fprintf(stderr, "apt: deb 行没有组件(至少写 main): %s\n", line);
            continue;
        }
        nsrc++;
    }
    fclose(f);
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
        fprintf(stderr, "apt: 下载 %s 失败: %s (code=%d)%s\n", url, r.error, rc,
                rc == 60 ? " (HTTPS 证书校验失败)" : "");
    return rc;
}

/* ------------------------- 索引 ------------------------- */
struct pkg {
    char *name, *version, *arch, *depends, *predepends, *provides;
    char *filename, *sha256, *size, *shortdesc, *section, *priority;
    int src;                                  /* 属于第几个源(信任判断用) */
};
static struct pkg pkgs[MAX_PKG];
static int npkg;

static char *fld(char *stanza, const char *key)
{
    size_t kl = strlen(key);
    char *p = stanza;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        if (llen > kl + 1 && !strncasecmp(p, key, kl) && p[kl] == ':') {
            char *v = p + kl + 1;
            while (*v == ' ')
                v++;
            size_t vl = (size_t)(p + llen - v);
            while (vl && v[vl - 1] == '\r')
                vl--;
            return xstrndup(v, vl);
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return NULL;
}

/* 缓存文件名: <源序号>_<组件序号>_<架构>_Packages —— 带组件与架构才不会
 * 互相顶掉(早先只按源编号命名, 多组件的源只剩最后一个组件的包)。 */
static void cache_name(char *out, size_t n, int si, int ci, const char *arch)
{
    char p[700];
    p_lists(p, sizeof p);
    snprintf(out, n, "%s/%d_%d_%s_Packages", p, si, ci, arch);
}

/* 把一个 Packages 文件按"空行分段"解析进 pkgs[]。
 * 自己逐行切: strtok("\n\n") 的字符集里两个 \n 会并成一个, 会把**每一行**
 * 当成一段(dpkg 那边踩过, 症状是字段全丢)。 */
static int index_load(const char *fp, int si)
{
    long len;
    unsigned char *b = pc_slurp(fp, &len);
    if (!b)
        return -1;
    if (len > IDX_MAX_BYTES) {
        fprintf(stderr, "apt: %s 有 %ld 字节, 超过上限 %ld —— 这一层是给小型"
                        "仓库写的, 大站请用真 apt\n", fp, len, IDX_MAX_BYTES);
        free(b);
        return -1;
    }
    char *line = (char *)b;
    char *cur = NULL;
    size_t curlen = 0, cap = 0;
    for (;;) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        int blank = (ll == 0 || (ll == 1 && line[0] == '\r'));
        if (blank) {
            if (cur && curlen && npkg < MAX_PKG) {
                cur[curlen] = 0;
                char *nm = fld(cur, "Package");
                if (nm) {
                    struct pkg *k = &pkgs[npkg++];
                    memset(k, 0, sizeof *k);
                    k->name = nm;
                    k->version = fld(cur, "Version");
                    k->arch = fld(cur, "Architecture");
                    k->depends = fld(cur, "Depends");
                    k->predepends = fld(cur, "Pre-Depends");
                    k->provides = fld(cur, "Provides");
                    k->filename = fld(cur, "Filename");
                    k->sha256 = fld(cur, "SHA256");
                    k->size = fld(cur, "Size");
                    k->section = fld(cur, "Section");
                    k->priority = fld(cur, "Priority");
                    k->shortdesc = fld(cur, "Description");
                    k->src = si;
                } else
                    npkg--;
            }
            curlen = 0;
            if (!nl)
                break;
            line = nl + 1;
            continue;
        }
        if (curlen + ll + 2 > cap) {
            cap = (curlen + ll + 2) * 2;
            char *t = realloc(cur, cap);
            if (!t)
                break;
            cur = t;
        }
        if (curlen)
            cur[curlen++] = '\n';
        memcpy(cur + curlen, line, ll);
        curlen += ll;
        if (curlen && cur[curlen - 1] == '\r')
            curlen--;
        if (!nl)
            break;
        line = nl + 1;
    }
    free(cur);
    free(b);
    return 0;
}

/* 缓存目录里已有哪些索引文件(名字本身带 源/组件/架构 信息)。
 * 顺手 read_sources(): 包记录里的 src 下标要拿去查"这个源可不可信"和
 * 拼下载 URL —— 忘了读源的话 srcs[] 全零, 表现成"下载 /pool/xxx.deb"
 * 和"来自未标 trusted=yes 的源 "(空基址)。 */
static int index_load_dir(void)
{
    char p[700];
    read_sources();
    p_lists(p, sizeof p);
    DIR *d = opendir(p);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 9 || strcmp(e->d_name + l - 9, "_Packages"))
            continue;
        char fp[800];
        snprintf(fp, sizeof fp, "%s/%s", p, e->d_name);
        /* 文件名开头是 "<源序号>_" */
        int si = atoi(e->d_name);
        index_load(fp, si);
    }
    closedir(d);
    return npkg;
}

/* ------------------------- 已装状态(dpkg 的 status) ------------------------- */
struct inst {
    char *name, *version, *provides, *depends;
};
static struct inst inst[2048];
static int ninst;

static void load_installed(void)
{
    char p[600];
    p_status(p, sizeof p);
    long len;
    unsigned char *b = pc_slurp(p, &len);
    ninst = 0;
    if (!b)
        return;
    char *line = (char *)b;
    char *cur = NULL;
    size_t curlen = 0, cap = 0;
    for (;;) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        int blank = (ll == 0 || (ll == 1 && line[0] == '\r'));
        if (blank) {
            if (cur && curlen && ninst < 2048) {
                cur[curlen] = 0;
                char *st = fld(cur, "Status");
                char *nm = fld(cur, "Package");
                /* fld 已经把冒号后的空格吃掉了, 找 " install ok installed"
                 * (带前导空格)永远找不到 —— 于是 apt 以为系统里一个包都没装,
                 * 表现成"刚装完就 remove 却说没装过"。 */
                if (nm && st && strstr(st, "install ok installed")) {
                    inst[ninst].name = nm;
                    inst[ninst].version = fld(cur, "Version");
                    inst[ninst].provides = fld(cur, "Provides");
                    inst[ninst].depends = fld(cur, "Depends");
                    nm = NULL;
                    ninst++;
                }
                free(nm);
                free(st);
            }
            curlen = 0;
            if (!nl)
                break;
            line = nl + 1;
            continue;
        }
        if (curlen + ll + 2 > cap) {
            cap = (curlen + ll + 2) * 2;
            char *t = realloc(cur, cap);
            if (!t)
                break;
            cur = t;
        }
        if (curlen)
            cur[curlen++] = '\n';
        memcpy(cur + curlen, line, ll);
        curlen += ll;
        if (curlen && cur[curlen - 1] == '\r')
            curlen--;
        if (!nl)
            break;
        line = nl + 1;
    }
    free(cur);
    free(b);
}

/* 第 i 个已装条目是否提供这个名字 */
static int entry_provides(const struct inst *e, const char *name)
{
    if (!strcmp(e->name, name))
        return 1;
    if (!e->provides)
        return 0;
    char *p = strdup(e->provides);
    char *save = NULL;
    int hit = 0;
    for (char *t = strtok_r(p, ",", &save); t && !hit;
         t = strtok_r(NULL, ",", &save)) {
        while (*t == ' ')
            t++;
        size_t l = strcspn(t, " (");
        if (l == strlen(name) && !strncmp(t, name, l))
            hit = 1;
    }
    free(p);
    return hit;
}

/* 名字是否被某个已装包"提供"(包名或 Provides 里的名字) */
static const struct inst *inst_provides(const char *name)
{
    for (int i = 0; i < ninst; i++)
        if (entry_provides(&inst[i], name))
            return &inst[i];
    return NULL;
}

/* ------------------------- 依赖串 ------------------------- */
struct dep_alt {
    char name[128];
    char op[8];
    char ver[128];
};
struct dep_item {
    struct dep_alt alt[MAX_ALT];
    int nalt;
};

static void parse_one_alt(const char *s, struct dep_alt *a)
{
    while (*s == ' ')
        s++;
    size_t i = 0;
    while (s[i] && s[i] != ' ' && s[i] != '(')
        i++;
    snprintf(a->name, sizeof a->name, "%.*s", (int)i, s);
    size_t l = strlen(a->name);
    while (l && (a->name[l - 1] == ' ' || a->name[l - 1] == '\t'))
        a->name[--l] = 0;
    const char *p = s + i;
    while (*p == ' ')
        p++;
    if (*p != '(')
        return;
    p++;
    while (*p == ' ')
        p++;
    size_t j = 0;
    while (p[j] && p[j] != ' ' && p[j] != ')')
        j++;
    snprintf(a->op, sizeof a->op, "%.*s", (int)(j > 7 ? 7 : j), p);
    p += j;
    while (*p == ' ')
        p++;
    j = 0;
    while (p[j] && p[j] != ')')
        j++;
    snprintf(a->ver, sizeof a->ver, "%.*s", (int)(j > 127 ? 127 : j), p);
}

static int parse_deps(const char *s, struct dep_item *out, int max)
{
    int n = 0;
    if (!s || !*s)
        return 0;
    char *dup = strdup(s);
    if (!dup)
        return 0;
    char *save = NULL;
    for (char *grp = strtok_r(dup, ",", &save); grp && n < max;
         grp = strtok_r(NULL, ",", &save)) {
        struct dep_item *it = &out[n];
        memset(it, 0, sizeof *it);
        char *s2 = NULL;
        for (char *alt = strtok_r(grp, "|", &s2); alt && it->nalt < MAX_ALT;
             alt = strtok_r(NULL, "|", &s2)) {
            char *q = alt;
            while (*q == ' ')
                q++;
            if (!*q)
                continue;
            parse_one_alt(q, &it->alt[it->nalt++]);
        }
        if (it->nalt)
            n++;
    }
    free(dup);
    return n;
}

/* 索引里该名字的最佳候选(架构可用且版本最高) */
static struct pkg *best_candidate(const char *name)
{
    struct pkg *best = NULL;
    for (int i = 0; i < npkg; i++) {
        if (!pkgs[i].name || strcmp(pkgs[i].name, name))
            continue;
        if (pkgs[i].arch && strcmp(pkgs[i].arch, "all") &&
            strcmp(pkgs[i].arch, host_deb_arch()))
            continue;
        if (!best)
            best = &pkgs[i];
        else if (best->version && pkgs[i].version &&
                 pc_deb_vercmp(pkgs[i].version, best->version) > 0)
            best = &pkgs[i];
    }
    return best;
}

static int alt_satisfied(const struct dep_alt *a, struct pkg **sel, int n)
{
    const struct inst *in = inst_provides(a->name);
    if (in && in->version &&
        (!a->op[0] || pc_deb_ver_match(in->version, a->op, a->ver)))
        return 1;
    for (int i = 0; i < n; i++) {
        struct pkg *k = sel[i];
        if (!k->name || strcmp(k->name, a->name))
            continue;
        if (!a->op[0] || pc_deb_ver_match(k->version ?: "", a->op, a->ver))
            return 1;
    }
    return 0;
}

/* 依赖闭包: 在选择集上反复展开, 直到没有新包要加。返回选择数, <0 失败。 */
static int resolve(struct pkg **sel, const char **want, int nwant)
{
    int n = 0;
    for (int i = 0; i < nwant; i++) {
        struct pkg *k = best_candidate(want[i]);
        if (!k) {
            fprintf(stderr, "apt: 找不到包 %s(源里没有? 先 apt update?)\n",
                    want[i]);
            return -1;
        }
        int already = 0;
        for (int j = 0; j < n; j++)
            if (sel[j] == k)
                already = 1;
        if (!already)
            sel[n++] = k;
    }
    for (int pass = 0; pass < 64; pass++) {
        int added = 0;
        for (int i = 0; i < n; i++) {
            struct pkg *k = sel[i];
            struct dep_item deps[MAX_DEPS * 2];
            int nd = parse_deps(k->depends, deps, MAX_DEPS * 2);
            nd += parse_deps(k->predepends, deps + nd, MAX_DEPS * 2 - nd);
            for (int d = 0; d < nd; d++) {
                int ok = 0;
                for (int a = 0; a < deps[d].nalt && !ok; a++)
                    if (alt_satisfied(&deps[d].alt[a], sel, n))
                        ok = 1;
                if (ok)
                    continue;
                struct pkg *pick = NULL;
                for (int a = 0; a < deps[d].nalt && !pick; a++) {
                    struct pkg *c = best_candidate(deps[d].alt[a].name);
                    if (!c)
                        continue;
                    if (deps[d].alt[a].op[0] &&
                        !pc_deb_ver_match(c->version ?: "", deps[d].alt[a].op,
                                          deps[d].alt[a].ver))
                        continue;
                    pick = c;
                }
                if (!pick) {
                    fprintf(stderr,
                            "apt: 依赖无法满足: %s 需要 %s(没有可用候选)\n",
                            k->name, deps[d].alt[0].name);
                    return -1;
                }
                int already = 0;
                for (int j = 0; j < n; j++)
                    if (sel[j] == pick)
                        already = 1;
                if (already)
                    continue;
                if (n >= MAX_PKG) {
                    fprintf(stderr, "apt: 选择集超过上限 %d\n", MAX_PKG);
                    return -1;
                }
                sel[n++] = pick;
                added = 1;
            }
        }
        if (!added)
            return n;
    }
    fprintf(stderr, "apt: 依赖展开超过 64 轮(循环依赖?)\n");
    return -1;
}

/* ------------------------- 调 dpkg ------------------------- */
static int run_dpkg(char **av)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "apt: fork 失败: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(av[0], av);
        execvp(av[0], av);                   /* 不在 /bin 时按 PATH 再找一次 */
        fprintf(stderr, "apt: 跑不了 %s —— 装包必须靠它: %s\n",
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

static int dpkg_install(char **files, int n)
{
    char *av[80];
    int i = 0;
    av[i++] = dpkg_path;
    if (opt_root[0]) {
        av[i++] = "--root";
        av[i++] = opt_root;
    }
    av[i++] = "-i";
    for (int j = 0; j < n && i < 76; j++)
        av[i++] = files[j];
    av[i] = NULL;
    return run_dpkg(av);
}

static int dpkg_remove(const char *flag, char **names, int n)
{
    char *av[80];
    int i = 0;
    av[i++] = dpkg_path;
    if (opt_root[0]) {
        av[i++] = "--root";
        av[i++] = opt_root;
    }
    av[i++] = (char *)flag;
    for (int j = 0; j < n && i < 76; j++)
        av[i++] = names[j];
    av[i] = NULL;
    return run_dpkg(av);
}

/* ------------------------- update ------------------------- */
static int do_update(void)
{
    read_sources();
    if (!nsrc) {
        char p[600];
        p_sources(p, sizeof p);
        fprintf(stderr, "apt: %s 里没有一条可用的 deb 源\n", p);
        return 100;
    }
    char lp[700];
    p_lists(lp, sizeof lp);
    if (pc_mkdirs(lp, 0755) < 0) {
        fprintf(stderr, "apt: 建 %s 失败: %s\n", lp, strerror(errno));
        return 100;
    }
    int fetched = 0, errs = 0;
    for (int i = 0; i < nsrc; i++) {
        struct src *sp = &srcs[i];
        for (int c = 0; c < sp->ncomp; c++) {
            for (int a = 0; a < sp->narch; a++) {
                char fp[760], gz[780], url[900];
                cache_name(fp, sizeof fp, i, c, sp->arch[a]);
                snprintf(gz, sizeof gz, "%s.gz", fp);
                int got = 0;
                snprintf(url, sizeof url,
                         "%s/dists/%s/%s/binary-%s/Packages.gz", sp->base,
                         sp->dist, sp->comp[c], sp->arch[a]);
                if (download(url, gz) == 0) {
                    long len;
                    unsigned char *b = pc_slurp(gz, &len);
                    unsigned char *out = NULL;
                    long olen = 0;
                    if (b && pc_gunzip(b, len, &out, &olen) == 0) {
                        unlink(fp);
                        if (pc_write_file(fp, out, olen, 0644) == 0)
                            got = 1;
                        free(out);
                    }
                    free(b);
                    unlink(gz);
                }
                if (!got) {
                    /* 小型仓库常只提供未压缩的 Packages */
                    snprintf(url, sizeof url,
                             "%s/dists/%s/%s/binary-%s/Packages", sp->base,
                             sp->dist, sp->comp[c], sp->arch[a]);
                    unlink(fp);
                    if (download(url, fp) == 0)
                        got = 1;
                }
                if (got) {
                    printf("已获取 %d:%s %s/%s binary-%s\n", i, sp->base,
                           sp->dist, sp->comp[c], sp->arch[a]);
                    fetched++;
                } else {
                    fprintf(stderr, "apt: 源 %d %s 的 %s/%s binary-%s "
                                    "取不到索引(跳过)\n", i, sp->base,
                            sp->dist, sp->comp[c], sp->arch[a]);
                    errs++;
                }
            }
        }
    }
    printf("已拉取 %d 份索引", fetched);
    if (errs)
        printf(", %d 份取不到", errs);
    printf("\n");
    return fetched ? 0 : 100;
}

/* ------------------------- install ------------------------- */
static int confirm(void)
{
    if (opt_yes)
        return 1;
    if (!isatty(0)) {
        fprintf(stderr, "apt: 非交互终端要装包请显式加 -y(--yes)\n");
        return 0;
    }
    printf("要继续吗? [Y/n] ");
    fflush(stdout);
    char line[128];
    if (!fgets(line, sizeof line, stdin))
        return 0;
    return (line[0] == 'y' || line[0] == 'Y' || line[0] == '\n' || !line[0]);
}

static int do_install(char **want, int nwant)
{
    load_installed();
    index_load_dir();
    if (!npkg) {
        fprintf(stderr, "apt: 索引是空的 —— 先跑 apt update\n");
        return 100;
    }
    struct pkg **sel = calloc(MAX_PKG, sizeof *sel);
    if (!sel)
        return 100;
    int nsel = resolve(sel, (const char **)want, nwant);
    if (nsel < 0) {
        free(sel);
        return 100;
    }
    printf("下列 %d 个包将被安装:\n", nsel);
    for (int i = 0; i < nsel; i++)
        printf("  %s %s %s\n", sel[i]->name, sel[i]->version ?: "?",
               sel[i]->arch ?: "?");
    if (!confirm()) {
        free(sel);
        return 1;
    }
    if (!opt_allow_untrusted) {
        for (int i = 0; i < nsel; i++) {
            if (!srcs[sel[i]->src].trusted) {
                fprintf(stderr,
                        "apt: 拒绝安装 %s: 它来自未标 trusted=yes 的源 %s。\n"
                        "     本实现**不做 GPG 签名校验**, 这道闸门就是替代手段:\n"
                        "     要么在 sources.list 那行加 [trusted=yes],"
                        " 要么显式 --allow-unauthenticated。\n",
                        sel[i]->name, srcs[sel[i]->src].base);
                free(sel);
                return 100;
            }
        }
    }
    char ad[700];
    p_archives(ad, sizeof ad);
    pc_mkdirs(ad, 0755);
    char **files = calloc((size_t)nsel, sizeof *files);
    if (!files) {
        free(sel);
        return 100;
    }
    int nfile = 0, rc = 0;
    for (int i = 0; i < nsel && !rc; i++) {
        struct pkg *k = sel[i];
        if (!k->filename) {
            fprintf(stderr, "apt: %s 的索引记录里没有 Filename 字段\n",
                    k->name);
            rc = 100;
            break;
        }
        char url[1000];
        snprintf(url, sizeof url, "%s/%s", srcs[k->src].base, k->filename);
        if (opt_print_uris) {
            printf("%s %s %s\n", url, k->filename, k->sha256 ?: "-");
            continue;
        }
        char dest[900];
        snprintf(dest, sizeof dest, "%s/%s", ad, pc_basename(k->filename));
        struct stat st;
        int reuse = 0;
        if (stat(dest, &st) == 0) {
            if (!k->sha256)
                reuse = k->size ? ((long)st.st_size ==
                                   strtol(k->size, NULL, 10)) : 1;
            else {
                char hex[65];
                if (pc_file_sha256_hex(dest, hex) == 0 &&
                    !strcasecmp(hex, k->sha256))
                    reuse = 1;
            }
        }
        if (!reuse) {
            printf("获取 %s\n", url);
            if (download(url, dest) != 0) {
                unlink(dest);
                rc = 100;
                break;
            }
        }
        /* 完整性核对: 索引声明的 Size / SHA256 必须对上 */
        if (stat(dest, &st) != 0) {
            fprintf(stderr, "apt: %s 下载后不见了\n", dest);
            rc = 100;
            break;
        }
        if (k->size && (long)st.st_size != strtol(k->size, NULL, 10)) {
            fprintf(stderr, "apt: %s 大小不符(索引 %s, 实得 %ld) ——"
                            " 删掉缓存, 不交给 dpkg\n", dest, k->size,
                    (long)st.st_size);
            unlink(dest);
            rc = 100;
            break;
        }
        if (k->sha256) {
            char hex[65];
            if (pc_file_sha256_hex(dest, hex) != 0) {
                fprintf(stderr, "apt: 算不出 %s 的 SHA256\n", dest);
                rc = 100;
                break;
            }
            if (strcasecmp(hex, k->sha256)) {
                fprintf(stderr, "apt: %s 的 SHA256 与索引不符(索引 %s,"
                                " 实得 %s) —— 删掉缓存, 不交给 dpkg\n",
                        pc_basename(k->filename), k->sha256, hex);
                unlink(dest);
                rc = 100;
                break;
            }
        }
        files[nfile++] = strdup(dest);
    }
    if (rc || opt_print_uris) {
        for (int i = 0; i < nfile; i++)
            free(files[i]);
        free(files);
        free(sel);
        return rc;
    }
    if (opt_no_download) {
        printf("apt: --no-download, 到此为止(没调 dpkg)\n");
        for (int i = 0; i < nfile; i++)
            free(files[i]);
        free(files);
        free(sel);
        return 0;
    }
    printf("正在交给 dpkg 解包安装...\n");
    int dr = dpkg_install(files, nfile);
    for (int i = 0; i < nfile; i++)
        free(files[i]);
    free(files);
    free(sel);
    if (dr != 0) {
        fprintf(stderr, "apt: dpkg 返回 %d(它的错误在上面)\n", dr);
        return dr;
    }
    printf("已安装 %d 个包\n", nfile);
    return 0;
}

/* ------------------------- remove/purge ------------------------- */
/* 反向依赖保护用的两个小判断 */
static int in_list(char **names, int n, const char *what)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], what))
            return 1;
    return 0;
}

static int do_remove(char **names, int n, const char *flag)
{
    load_installed();
    if (!n) {
        fprintf(stderr, "apt: %s 需要包名\n", flag);
        return 100;
    }
    for (int i = 0; i < n; i++)
        if (!inst_provides(names[i])) {
            fprintf(stderr, "apt: 包 %s 没装过(状态库里查不到)\n", names[i]);
            return 100;
        }
    /* 反向依赖保护: 还有别的已装包 Depends 上要删的名字, 且删完之后再没人
     * 提供它, 就不许删。没有这一步, "apt remove libc6" 会把脚下依赖它的
     * 包全清空。 */
    int blocked = 0;
    for (int j = 0; j < ninst && !blocked; j++) {
        if (in_list(names, n, inst[j].name) || !inst[j].depends)
            continue;
        struct dep_item deps[MAX_DEPS];
        int nd = parse_deps(inst[j].depends, deps, MAX_DEPS);
        for (int d = 0; d < nd && !blocked; d++) {
            const char *hit = NULL;
            for (int a = 0; a < deps[d].nalt && !hit; a++)
                if (in_list(names, n, deps[d].alt[a].name))
                    hit = deps[d].alt[a].name;
            if (!hit)
                continue;
            int still = 0;
            for (int k = 0; k < ninst && !still; k++) {
                if (k == j || in_list(names, n, inst[k].name))
                    continue;
                if (entry_provides(&inst[k], hit))
                    still = 1;
            }
            if (!still) {
                fprintf(stderr, "apt: 拒绝删除 %s: 已装的 %s 还 Depends 它\n",
                        hit, inst[j].name);
                blocked = 1;
            }
        }
    }
    if (blocked) {
        fprintf(stderr, "apt: 要连依赖方一起删, 请把它们也写进同一条命令\n");
        return 100;
    }
    int rc = dpkg_remove(flag, names, n);
    if (rc == 0)
        printf("apt: 完成\n");
    return rc;
}

/* ------------------------- 查询类 ------------------------- */
static void print_pkg_record(const struct pkg *k)
{
    printf("Package: %s\nVersion: %s\nArchitecture: %s\n",
           k->name, k->version ?: "?", k->arch ?: "?");
    if (k->depends)
        printf("Depends: %s\n", k->depends);
    if (k->predepends)
        printf("Pre-Depends: %s\n", k->predepends);
    if (k->provides)
        printf("Provides: %s\n", k->provides);
    if (k->section)
        printf("Section: %s\n", k->section);
    if (k->priority)
        printf("Priority: %s\n", k->priority);
    if (k->size)
        printf("Installed-Size: %s\n", k->size);
    if (k->sha256)
        printf("SHA256: %s\n", k->sha256);
    if (k->filename)
        printf("Filename: %s\n", k->filename);
    printf("Source: %s (trusted=%s)\n", srcs[k->src].base,
           srcs[k->src].trusted ? "yes" : "no");
    if (k->shortdesc)
        printf("Description: %s\n", k->shortdesc);
    printf("\n");
}

static int do_show(char **names, int n)
{
    index_load_dir();
    int rc = 0;
    for (int i = 0; i < n; i++) {
        struct pkg *k = best_candidate(names[i]);
        if (!k) {
            fprintf(stderr, "apt: 没有包 %s\n", names[i]);
            rc = 100;
            continue;
        }
        print_pkg_record(k);
    }
    return rc;
}

static int do_search(char **names, int n)
{
    index_load_dir();
    if (!npkg) {
        fprintf(stderr, "apt: 索引是空的 —— 先跑 apt update\n");
        return 100;
    }
    int hits = 0;
    for (int i = 0; i < npkg; i++) {
        struct pkg *k = &pkgs[i];
        for (int j = 0; j < n; j++) {
            if ((k->name && strcasestr(k->name, names[j])) ||
                (k->shortdesc && strcasestr(k->shortdesc, names[j]))) {
                printf("%s - %s (%s)\n", k->name,
                       k->shortdesc ? k->shortdesc : "", k->version ?: "?");
                hits++;
                break;
            }
        }
    }
    if (!hits)
        printf("apt: 没有匹配的包\n");
    return 0;
}

static int do_list(char **pat, int np, int installed_only)
{
    load_installed();
    index_load_dir();
    if (installed_only) {
        for (int i = 0; i < ninst; i++) {
            if (np && !strcasestr(inst[i].name, pat[0]))
                continue;
            printf("%s/installed %s\n", inst[i].name,
                   inst[i].version ?: "?");
        }
        return 0;
    }
    for (int i = 0; i < npkg; i++) {
        struct pkg *k = &pkgs[i];
        if (np && !strcasestr(k->name, pat[0]))
            continue;
        const struct inst *in = inst_provides(k->name);
        printf("%s/%s %s\n", k->name, in ? "installed" : "not-installed",
               k->version ?: "?");
    }
    return 0;
}

static int do_policy(char **names, int n)
{
    load_installed();
    read_sources();
    index_load_dir();
    if (!n) {
        printf("apt %s: %d 个源\n", APT_VERSION, nsrc);
        for (int i = 0; i < nsrc; i++) {
            printf("  %d: %s %s %s(trusted=%s)\n", i, srcs[i].base,
                   srcs[i].dist, srcs[i].comp[0],
                   srcs[i].trusted ? "yes" : "no");
        }
        return 0;
    }
    for (int i = 0; i < n; i++) {
        const struct inst *in = inst_provides(names[i]);
        struct pkg *k = best_candidate(names[i]);
        printf("%s:\n", names[i]);
        printf("  已装: %s\n", in && in->version ? in->version : "(无)");
        printf("  候选: %s\n", k && k->version ? k->version : "(无)");
        for (int j = 0; j < npkg; j++)
            if (pkgs[j].name && !strcmp(pkgs[j].name, names[i]))
                printf("    %s  %s\n", pkgs[j].version ?: "?",
                       srcs[pkgs[j].src].base);
    }
    return 0;
}

static int do_clean(void)
{
    char p[700];
    p_archives(p, sizeof p);
    DIR *d = opendir(p);
    if (!d) {
        printf("apt: 没有下载缓存(%s)\n", p);
        return 0;
    }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char f[800];
        snprintf(f, sizeof f, "%s/%s", p, e->d_name);
        if (unlink(f) == 0)
            n++;
    }
    closedir(d);
    printf("apt: 清掉 %d 个下载缓存文件\n", n);
    return 0;
}

static void usage(void)
{
    printf("apt %s (Parlz 移植实现; 解包安装由 dpkg 完成)\n", APT_VERSION);
    printf("用法: apt [选项] 动作 [参数]\n");
    printf("  update                     拉 sources.list 里所有源的索引\n");
    printf("  install <包>...            求解依赖 → 下载 → 校验 → dpkg -i\n");
    printf("  remove|purge <包>...        卸载(带反向依赖保护)\n");
    printf("  search <关键字>...           在名字与描述里找\n");
    printf("  show <包>...               打印索引记录\n");
    printf("  list [--installed] [模式]   列包\n");
    printf("  policy [包]                已装/候选版本与来源\n");
    printf("  clean                      清下载缓存\n");
    printf("选项: --root <目录> -y|--yes --allow-unauthenticated\n");
    printf("      --print-uris --no-download --dpkg <路径> --version\n");
    printf("源: /etc/apt/sources.list —— deb [trusted=yes] <URI> <发行> <组件>...\n");
    printf("索引: <URI>/dists/<发行>/<组件>/binary-<架构>/Packages[.gz]\n");
}

int main(int argc, char **argv)
{
    const char *act = NULL;
    char *args[64];
    int nargs = 0;
    int installed_only = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--version")) {
            printf("apt %s\n", APT_VERSION);
            return 0;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(a, "--root") || !strcmp(a, "--dpkg")) {
            int isroot = !strcmp(a, "--root");
            if (++i >= argc) {
                fprintf(stderr, "apt: %s 需要参数\n", a);
                return 100;
            }
            char *dst = isroot ? opt_root : dpkg_path;
            size_t dn = isroot ? sizeof opt_root : sizeof dpkg_path;
            snprintf(dst, dn, "%s", argv[i]);
            if (isroot) {
                char *sl = dst + strlen(dst) - 1;
                while (sl > dst && *sl == '/')
                    *sl-- = 0;
            }
            continue;
        }
        if (!strcmp(a, "-y") || !strcmp(a, "--yes") ||
            !strcmp(a, "--assume-yes")) { opt_yes = 1; continue; }
        if (!strcmp(a, "--allow-unauthenticated")) {
            opt_allow_untrusted = 1;
            continue;
        }
        if (!strcmp(a, "--print-uris")) { opt_print_uris = 1; continue; }
        if (!strcmp(a, "--no-download")) { opt_no_download = 1; continue; }
        if (!strcmp(a, "--installed")) { installed_only = 1; continue; }
        if (!strcmp(a, "-f") || !strcmp(a, "--fix-broken")) {
            fprintf(stderr, "apt: --fix-broken 没实现(依赖不满足就直接失败)\n");
            return 100;
        }
        if (!strcmp(a, "-o") || !strcmp(a, "--option")) {
            if (i + 1 < argc)
                i++;
            fprintf(stderr, "apt: 忽略 -o 配置项(只认命令行开关)\n");
            continue;
        }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "apt: 不认的选项 %s(见 apt --help)\n", a);
            return 100;
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
        return 100;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!strcmp(act, "update"))
        return do_update();
    if (!strcmp(act, "install") || !strcmp(act, "add")) {
        if (!nargs) {
            fprintf(stderr, "apt: install 需要包名\n");
            return 100;
        }
        return do_install(args, nargs);
    }
    if (!strcmp(act, "remove") || !strcmp(act, "purge"))
        return do_remove(args, nargs, !strcmp(act, "purge") ? "-P" : "-r");
    if (!strcmp(act, "search"))
        return do_search(args, nargs);
    if (!strcmp(act, "show"))
        return do_show(args, nargs);
    if (!strcmp(act, "list"))
        return do_list(args, nargs, installed_only);
    if (!strcmp(act, "policy"))
        return do_policy(args, nargs);
    if (!strcmp(act, "clean"))
        return do_clean();
    fprintf(stderr, "apt: 未实现的动作 %s\n", act);
    return 100;
}
