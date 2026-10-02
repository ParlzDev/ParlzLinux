// SPDX-License-Identifier: GPL-2.0-or-later
/* dpkg.c - Parlz 移植的 dpkg(.deb 包的底层安装器)。
 *
 * 包格式(.deb = "2.0" ar 档): ar 成员 debian-binary + control.tar.* + data.tar.*
 *   control.tar 里是 control(字段文本)、md5sums、conffiles、preinst/postinst/
 *   prerm/postrm 维护脚本; data.tar 里是装在 root 下的文件树。
 * 状态库: <admindir=/var/lib/dpkg>/status 逐包一个字段 stanza,
 *   info/<pkg>.list 记已落盘路径(卸载按它删), info/<pkg>.<script> 存维护脚本。
 *
 * 子命令(与 dpkg 的常用面一致, 只实现这些):
 *   -i|--install <file.deb>...      解包并安装(preinst → data → 状态 → postinst)
 *   -r|--remove <pkg>...            卸载(按 .list 删, 配置留着)
 *   -P|--purge <pkg>...             卸载 + 清配置与状态条目
 *   -l|--list [模式]                已装包
 *   -s|--status <pkg>               打印状态 stanza
 *   -L|--listfiles <pkg>            已装文件清单
 *   -c|--contents <file.deb>        打印包里 data 的成员表
 *   -I|--info <file.deb> [字段]     打印包 control 字段
 *   --compare-versions <a> <op> <b> Debian 版本比较, 真=0 假=1
 *   --root <dir> / --instdir <dir> / --admindir <dir>(也认 DPKG_* 环境变量)
 *
 * 与 dpkg 的已知差别(不冒充全量实现):
 *   - 只解 gzip 与不压缩的 tar; .xz/.zst 负载明确报错而不是硬猜。
 *   - 不做触发器(triggers)、多架构并存、二进制索引、GPG 签名校验。
 *   - 维护脚本用 /bin/sh 跑, 超时 300 秒即放弃(不许静默长等)。
 * 约束(AGENTS.md): 静态、无 system()、覆盖文件先 unlink 再建、写完显式 chmod。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/utsname.h>

#include "pkgcore.h"

#define DEB_VERSION "1.0.0"          /* 本移植实现的版本串(dpkg --version 打印) */
#define SCRIPT_TIMEOUT 300           /* 维护脚本上限, 秒 */
#define MAX_STANZA 8192
#define MAX_PKGS 2048

/* ------------------------- 全局选项 ------------------------- */
static char opt_root[512];            /* 文件落盘的根(空=真实根) */
static char opt_adm[512];             /* 状态库目录 */
static int  force_arch, force_all, force_overwrite, no_scripts;

static void paths_init(void)
{
    const char *e;
    if (!opt_root[0]) {
        e = getenv("DPKG_ROOT");
        if (e && *e)
            snprintf(opt_root, sizeof opt_root, "%s", e);
    }
    if (!opt_adm[0]) {
        e = getenv("DPKG_ADMINDIR");
        if (e && *e)
            snprintf(opt_adm, sizeof opt_adm, "%s", e);
        else
            snprintf(opt_adm, sizeof opt_adm, "%s/var/lib/dpkg", opt_root);
    }
}

static void adm_path(char *out, size_t n, const char *leaf)
{
    snprintf(out, n, "%s/%s", opt_adm, leaf);
}

/* dpkg 用一把锁挡住并发安装(apt 会调 dpkg, 人也可能在另一个终端里调)。 */
static int take_lock(void)
{
    char p[600];
    pc_mkdirs(opt_adm, 0755);
    snprintf(p, sizeof p, "%s/lock", opt_adm);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    for (int i = 0; i < 120; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            return fd;
        if (i == 0)
            fprintf(stderr, "dpkg: 另一个 dpkg/apt 正在装包, 等锁...\n");
        usleep(500000);
    }
    fprintf(stderr, "dpkg: 取不到 %s 的锁(60 秒内没别的进程释放)\n", p);
    close(fd);
    return -1;
}

/* ------------------------- 状态库 ------------------------- */
struct pkg {
    char *name;
    char *version;
    char *arch;
    char *statusword;                 /* installed / config-files / not-installed */
    char *raw;                        /* 完整 stanza 文本(重写文件时原样落回) */
};

static struct pkg pkgs[MAX_PKGS];
static int npkg;

static char *field_of(const char *stanza, const char *key)
{
    if (!stanza)
        return NULL;
    size_t kl = strlen(key);
    const char *p = stanza;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        if (llen > kl + 1 && !strncasecmp(p, key, kl) && p[kl] == ':') {
            const char *v = p + kl + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t vl = (size_t)(p + llen - v);
            while (vl && v[vl - 1] == '\r')
                vl--;
            char *o = malloc(vl + 1);
            if (o) {
                memcpy(o, v, vl);
                o[vl] = 0;
            }
            return o;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return NULL;
}

static char *status_word(const char *stanza)
{
    char *s = field_of(stanza, "Status");
    if (!s)
        return strdup("not-installed");
    char *sp = strrchr(s, ' ');
    char *r = strdup(sp ? sp + 1 : s);
    free(s);
    return r;
}

static void pkg_free(struct pkg *k)
{
    free(k->name);
    free(k->version);
    free(k->arch);
    free(k->statusword);
    free(k->raw);
    memset(k, 0, sizeof *k);
}

static int status_load(void)
{
    char p[600];
    adm_path(p, sizeof p, "status");
    long len = 0;
    unsigned char *b = pc_slurp(p, &len);
    for (int i = 0; i < npkg; i++)
        pkg_free(&pkgs[i]);
    npkg = 0;
    if (!b)
        return 0;                       /* 没有状态库 = 一个包都没装 */
    /* stanza 之间用**空行**分隔。不能拿 strtok 切: "\n\n" 的字符集里
     * '\n' 只有一个, 切出来的是一段**行**而不是一条 stanza(踩过:
     * 结果 raw 只装着 "Package: x" 一行, Version/Status 全丢,
     * 于是 -l 没有 ii、-r 认为没装过、一个文件都不删)。 */
    char *txt = (char *)b;
    char *line = txt;
    char *cur = malloc((size_t)len + 1);
    if (!cur) {
        free(b);
        return -1;
    }
    size_t clen = 0;
    for (;;) {
        char *nl = strchr(line, '\n');
        size_t ll = nl ? (size_t)(nl - line) : strlen(line);
        int blank = (ll == 0 || (ll == 1 && line[0] == '\r'));
        if (blank && clen > 0) {
            cur[clen] = 0;
            struct pkg *k = NULL;
            if (npkg < MAX_PKGS)
                k = &pkgs[npkg];
            if (k) {
                memset(k, 0, sizeof *k);
                char *trimmed = cur;
                while (*trimmed == '\n')
                    trimmed++;
                k->raw = strdup(trimmed);
                k->name = field_of(trimmed, "Package");
                if (k->raw && k->name) {
                    k->version = field_of(trimmed, "Version");
                    k->arch = field_of(trimmed, "Architecture");
                    k->statusword = status_word(trimmed);
                    npkg++;
                } else {
                    pkg_free(k);
                }
            }
            clen = 0;
        } else if (!blank && clen + ll + 2 <= (size_t)len + 1) {
            if (clen)
                cur[clen++] = '\n';
            memcpy(cur + clen, line, ll);
            clen += ll;
            if (clen && cur[clen - 1] == '\r')
                clen--;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    if (clen > 0 && npkg < MAX_PKGS) {           /* 末尾没有空行也要收下 */
        cur[clen] = 0;
        struct pkg *k = &pkgs[npkg];
        memset(k, 0, sizeof *k);
        k->raw = strdup(cur);
        k->name = field_of(cur, "Package");
        if (k->raw && k->name) {
            k->version = field_of(cur, "Version");
            k->arch = field_of(cur, "Architecture");
            k->statusword = status_word(cur);
            npkg++;
        } else
            pkg_free(k);
    }
    free(cur);
    free(b);
    return npkg;
}

static struct pkg *pkg_find(const char *name)
{
    for (int i = 0; i < npkg; i++)
        if (pkgs[i].name && !strcmp(pkgs[i].name, name))
            return &pkgs[i];
    return NULL;
}

static int pkg_is_installed(const char *name)
{
    struct pkg *k = pkg_find(name);
    return k && k->statusword && !strcmp(k->statusword, "installed");
}

static void pkg_drop(const char *name)
{
    for (int i = 0; i < npkg; i++) {
        if (pkgs[i].name && !strcmp(pkgs[i].name, name)) {
            pkg_free(&pkgs[i]);
            pkgs[i] = pkgs[npkg - 1];
            memset(&pkgs[npkg - 1], 0, sizeof pkgs[npkg - 1]);
            npkg--;
            return;
        }
    }
}

/* 整份重写 status: 先写 .new + fsync, 再 rename, 最后 sync()。
 * 中途掉电不会把状态库写成空文件(空 = 系统认为什么都没装过)。 */
static int status_save(void)
{
    char p[600], t[640];
    adm_path(p, sizeof p, "status");
    snprintf(t, sizeof t, "%s.new", p);
    pc_mkdirs(opt_adm, 0755);
    FILE *f = fopen(t, "w");
    if (!f) {
        fprintf(stderr, "dpkg: 写 %s 失败: %s\n", t, strerror(errno));
        return -1;
    }
    for (int i = 0; i < npkg; i++)
        fprintf(f, "%s\n\n", pkgs[i].raw);
    int ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    fclose(f);
    if (!ok) {
        fprintf(stderr, "dpkg: 刷 %s 失败: %s\n", t, strerror(errno));
        unlink(t);
        return -1;
    }
    if (rename(t, p) != 0) {
        fprintf(stderr, "dpkg: 换 %s 失败: %s\n", p, strerror(errno));
        unlink(t);
        return -1;
    }
    sync();
    return 0;
}

static int status_add(const char *stanza)
{
    char *nm = field_of(stanza, "Package");
    if (!nm)
        return -1;
    struct pkg *k = pkg_find(nm);
    if (k)
        pkg_free(k);
    else {
        if (npkg >= MAX_PKGS) {
            free(nm);
            return -1;
        }
        k = &pkgs[npkg++];
    }
    memset(k, 0, sizeof *k);
    k->raw = strdup(stanza);
    k->name = nm;
    k->version = field_of(stanza, "Version");
    k->arch = field_of(stanza, "Architecture");
    k->statusword = status_word(stanza);
    if (!k->raw) {
        pkg_free(k);
        npkg--;
        return -1;
    }
    return 0;
}

/* ------------------------- info/ 下的伴生文件 ------------------------- */
static void info_path(char *out, size_t n, const char *pkg, const char *ext)
{
    char d[600];
    snprintf(d, sizeof d, "%s/info", opt_adm);
    pc_mkdirs(d, 0755);
    snprintf(out, n, "%s/%s.%s", d, pkg, ext);
}

/* ------------------------- .deb 容器解析 ------------------------- */
struct deb {
    unsigned char *img;
    long len;
    const unsigned char *ctrl;        /* control.tar 解压后的原始 tar 内存 */
    long ctrllen;
    int ctrl_owned;
    const unsigned char *data;
    long datalen;
    int data_owned;
    char ctrlname[64];
    char dataname[64];
};

static int pick_member(const unsigned char *img, long len,
                       const char **names, const unsigned char **raw,
                       long *rawlen, char *hit, size_t hn)
{
    for (int i = 0; names[i]; i++) {
        if (pc_ar_member(img, len, names[i], raw, rawlen) == 0) {
            snprintf(hit, hn, "%s", names[i]);
            return 0;
        }
    }
    hit[0] = 0;
    return -1;
}

static int unsupported_comp(const char *nm)
{
    /* "control.tar" / "control.tar.gz" 能解; ".xz" ".zst" ".bz2" 不能。
     * 必须在**解之前**判掉: 否则不认识的容器会被当成未压缩 tar 直接往下走,
     * 报错就变成一句看不出根因的"control.tar 里没有 control 文件"。 */
    const char *s = strstr(nm, ".tar");
    if (!s)
        return 1;
    s += 4;
    return !(s[0] == 0 || !strcmp(s, ".gz") || !strcmp(s, ".gzip"));
}

static int deb_open(struct deb *d, const char *path)
{
    long len;
    memset(d, 0, sizeof *d);
    d->img = pc_slurp(path, &len);
    if (!d->img) {
        fprintf(stderr, "dpkg: 读 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    d->len = len;
    const unsigned char *v;
    long vl;
    if (pc_ar_member(d->img, len, "debian-binary", &v, &vl) != 0) {
        fprintf(stderr, "dpkg: %s 不是 ar 档(缺 debian-binary), 不是 .deb 包\n",
                path);
        goto bad;
    }
    if (vl < 3 || strncmp((const char *)v, "2.0", 3) != 0) {
        fprintf(stderr, "dpkg: %s 的 debian-binary 版本是 '%.*s', 只支持 2.0\n",
                path, (int)(vl > 16 ? 16 : vl), v);
        goto bad;
    }
    static const char *ctrls[] = { "control.tar.gz", "control.tar",
                                   "control.tar.xz", "control.tar.zst", NULL };
    static const char *datas[] = { "data.tar.gz", "data.tar", "data.tar.xz",
                                   "data.tar.zst", "data.tar.bz2", NULL };
    const unsigned char *raw = NULL;
    long rawlen = 0;
    if (pick_member(d->img, len, ctrls, &raw, &rawlen,
                    d->ctrlname, sizeof d->ctrlname) < 0) {
        fprintf(stderr, "dpkg: %s 里没有 control.tar.*(本实现支持 "
                        "control.tar 与 control.tar.gz)\n", path);
        goto bad;
    }
    if (unsupported_comp(d->ctrlname)) {
        fprintf(stderr, "dpkg: %s 用了 %s, 本实现只解 gzip 与不压缩 tar;"
                        " 打包端请换 -Zgzip\n", path, d->ctrlname);
        goto bad;
    }
    if (pc_maybe_gunzip(raw, rawlen, (unsigned char **)&d->ctrl, &d->ctrllen,
                        &d->ctrl_owned) != 0) {
        fprintf(stderr, "dpkg: 解 %s 失败(gzip 流损坏)\n", d->ctrlname);
        goto bad;
    }
    if (pick_member(d->img, len, datas, &raw, &rawlen,
                    d->dataname, sizeof d->dataname) < 0) {
        fprintf(stderr, "dpkg: %s 里没有 data.tar.*\n", path);
        goto bad;
    }
    if (unsupported_comp(d->dataname)) {
        fprintf(stderr, "dpkg: %s 用了 %s, 本实现只解 gzip 与不压缩 tar;"
                        " 打包端请换 -Zgzip\n", path, d->dataname);
        goto bad;
    }
    if (pc_maybe_gunzip(raw, rawlen, (unsigned char **)&d->data, &d->datalen,
                        &d->data_owned) != 0) {
        fprintf(stderr, "dpkg: 解 %s 失败(gzip 流损坏)\n", d->dataname);
        goto bad;
    }
    if (d->datalen < 512) {
        fprintf(stderr, "dpkg: %s 的 data.tar 太小(%ld 字节), 包是空的或坏的\n",
                path, d->datalen);
        goto bad;
    }
    return 0;
bad:
    free(d->img);
    d->img = NULL;
    return -1;
}

static void deb_close(struct deb *d)
{
    if (d->ctrl_owned)
        free((void *)d->ctrl);
    if (d->data_owned)
        free((void *)d->data);
    free(d->img);
    memset(d, 0, sizeof *d);
}

/* 从 control.tar 里取一个成员的内容(NUL 结尾) */
struct ctrlget {
    const char *want;
    char *out;
};

static int ctrl_pick_cb(const struct pc_mem *m, void *ud)
{
    struct ctrlget *g = (struct ctrlget *)ud;
    const char *nm = m->name;
    if (!strncmp(nm, "./", 2))
        nm += 2;
    if (m->type == PC_REG && !strcmp(nm, g->want)) {
        g->out = malloc((size_t)m->size + 1);
        if (!g->out)
            return -1;
        memcpy(g->out, m->data, (size_t)m->size);
        g->out[m->size] = 0;
        return 1;                             /* 拿到了, 提前结束 */
    }
    return 0;
}

static char *ctrl_file(struct deb *d, const char *name)
{
    struct ctrlget g;
    memset(&g, 0, sizeof g);
    g.want = name;
    pc_tar_walk(d->ctrl, d->ctrllen, ctrl_pick_cb, &g);
    return g.out;
}

/* ------------------------- 架构 ------------------------- */
static const char *host_arch(void)
{
    static char a[64];
    if (a[0])
        return a;
    struct utsname u;
    if (uname(&u) == 0) {
        if (!strcmp(u.machine, "x86_64"))
            snprintf(a, sizeof a, "amd64");
        else if (!strcmp(u.machine, "aarch64"))
            snprintf(a, sizeof a, "arm64");
        else if (!strcmp(u.machine, "i386") || !strcmp(u.machine, "i686"))
            snprintf(a, sizeof a, "i386");
        else
            snprintf(a, sizeof a, "%s", u.machine);
    } else
        snprintf(a, sizeof a, "amd64");
    return a;
}

/* ------------------------- 维护脚本 ------------------------- */
static int run_script(const char *path, const char *arg)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;                             /* 没这个脚本: 正常 */
    if (no_scripts) {
        printf("dpkg: 跳过维护脚本 %s %s(--no-scripts)\n",
               pc_basename(path), arg);
        return 0;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "dpkg: fork 失败: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        char *av[4];
        av[0] = (char *)"sh";
        av[1] = (char *)path;
        av[2] = (char *)arg;
        av[3] = NULL;
        if (opt_root[0])
            setenv("DPKG_ROOT", opt_root, 1);
        execv("/bin/sh", av);
        /* /bin/sh 不存在时明确报错: 静默"安装成功"比报错更坏 */
        fprintf(stderr, "dpkg: 跑 %s 需要 /bin/sh, execv 失败: %s\n",
                path, strerror(errno));
        _exit(127);
    }
    int status = -1;
    long waited = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR))
            break;
        sleep(1);
        if (++waited >= SCRIPT_TIMEOUT) {
            fprintf(stderr, "dpkg: 维护脚本 %s %s 超过 %d 秒, 杀掉它(不静默长等)\n",
                    pc_basename(path), arg, SCRIPT_TIMEOUT);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        if (waited % 30 == 0)
            fprintf(stderr, "dpkg: 维护脚本 %s %s 已跑 %ld 秒...\n",
                    pc_basename(path), arg, waited);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    fprintf(stderr, "dpkg: 维护脚本 %s %s 失败(%s%d)\n", pc_basename(path), arg,
            WIFSIGNALED(status) ? "死于信号 " : "退出码 ",
            WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    return -1;
}

/* ------------------------- data.tar 抽取 + 清单登记 ------------------------- */
struct xctx {
    struct pc_extract *e;
    FILE *list;
    long nreg;
};

static int x_cb(const struct pc_mem *m, void *ud)
{
    struct xctx *x = (struct xctx *)ud;
    if (pc_extract_mem(x->e, m) != 0)
        return -1;
    char rel[1100];
    /* 清单记 instdir 相对路径(与 dpkg 的 info/<pkg>.list 一致):
     * root 为空时它与落盘路径相同。 */
    if (pc_member_path("", m->name, rel, sizeof rel) == 0 && rel[0] &&
        strcmp(rel, "/")) {
        fprintf(x->list, "%s\n", rel);
        x->nreg++;
    }
    return 0;
}

/* ------------------------- 安装 ------------------------- */
static int do_install(const char *path)
{
    struct deb d;
    if (deb_open(&d, path) < 0)
        return 2;

    char *control = ctrl_file(&d, "control");
    if (!control) {
        fprintf(stderr, "dpkg: %s 的 control.tar 里没有 control 文件\n", path);
        deb_close(&d);
        return 2;
    }
    char *name = field_of(control, "Package");
    char *version = field_of(control, "Version");
    char *arch = field_of(control, "Architecture");
    char *size = field_of(control, "Installed-Size");
    char *maint = field_of(control, "Maintainer");
    char *prio = field_of(control, "Priority");
    char *sect = field_of(control, "Section");
    char *pend = field_of(control, "Depends");
    char *pre = field_of(control, "Pre-Depends");
    char *conf = field_of(control, "Conflicts");
    char *mver = field_of(control, "Multi-Arch");
    char *desc = field_of(control, "Description");
    if (!name || !version) {
        fprintf(stderr, "dpkg: %s 的 control 缺 Package/Version 字段\n", path);
        goto fail_open;
    }
    if (arch && strcmp(arch, "all") && strcmp(arch, host_arch()) &&
        !force_arch && !force_all) {
        fprintf(stderr,
                "dpkg: 包 %s 是 %s 架构, 本机是 %s —— 拒绝安装"
                "(强行装加 --force-architecture)\n",
                name, arch, host_arch());
        goto fail_open;
    }
    status_load();
    if (pkg_is_installed(name) && !force_overwrite && !force_all) {
        struct pkg *old = pkg_find(name);
        if (old->version && pc_deb_vercmp(version, old->version) < 0) {
            fprintf(stderr,
                    "dpkg: 已装 %s %s 比要装的 %s 更新 —— 降级要 --force-downgrade\n",
                    name, old->version, version);
            goto fail_open;
        }
    }
    int lockfd = take_lock();
    if (lockfd < 0)
        goto fail_open;

    printf("dpkg: 准备解包 %s %s(%s, data=%s)\n", name, version,
           arch ? arch : "?", d.dataname);

    /* 1) control 与维护脚本落到 info/ */
    char ip[680];
    info_path(ip, sizeof ip, name, "control");
    {
        int fd = open(ip, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t w = write(fd, control, strlen(control));
            (void)w;
            close(fd);
        }
    }
    static const char *scr[] = { "preinst", "postinst", "prerm", "postrm", NULL };
    for (int i = 0; scr[i]; i++) {
        char *body = ctrl_file(&d, scr[i]);
        if (!body)
            continue;
        info_path(ip, sizeof ip, name, scr[i]);
        pc_write_file(ip, (unsigned char *)body, (long)strlen(body), 0755);
        free(body);
    }
    char *md5 = ctrl_file(&d, "md5sums");
    if (md5) {
        info_path(ip, sizeof ip, name, "md5sums");
        pc_write_file(ip, (unsigned char *)md5, (long)strlen(md5), 0644);
        free(md5);
    }
    char *cf = ctrl_file(&d, "conffiles");
    if (cf) {
        info_path(ip, sizeof ip, name, "conffiles");
        pc_write_file(ip, (unsigned char *)cf, (long)strlen(cf), 0644);
        free(cf);
    }

    /* 2) preinst —— 失败就中止, 一个文件都还没解 */
    info_path(ip, sizeof ip, name, "preinst");
    if (run_script(ip, pkg_is_installed(name) ? "upgrade" : "install") < 0) {
        fprintf(stderr, "dpkg: %s 的 preinst 失败, 中止安装(未解 data)\n", name);
        close(lockfd);
        goto fail_open;
    }

    /* 3) 解 data.tar 到 root, 同时写 .list */
    info_path(ip, sizeof ip, name, "list");
    unlink(ip);
    FILE *lf = fopen(ip, "w");
    if (!lf) {
        fprintf(stderr, "dpkg: 写 %s 失败: %s\n", ip, strerror(errno));
        close(lockfd);
        goto fail_open;
    }
    struct pc_extract *e;
    struct xctx x;
    x.list = lf;
    x.nreg = 0;
    if (pc_extract_init(&e, opt_root) < 0) {
        fclose(lf);
        close(lockfd);
        goto fail_open;
    }
    x.e = e;
    int wr = pc_tar_walk(d.data, d.datalen, x_cb, &x);
    long nmem = pc_extract_count(e);
    int nerr = pc_extract_errors(e);
    fclose(lf);
    if (wr > 0)
        wr = 0;                                /* x_cb 提前结束用的 1 不算错 */
    if (wr < 0 || nerr || nmem == 0) {
        fprintf(stderr, "dpkg: 解 %s 失败: 成员=%ld, 失败=%d, rc=%d"
                        "(包损坏或磁盘写满?)\n", d.dataname, nmem, nerr, wr);
        close(lockfd);
        goto fail_open;
    }
    printf("dpkg: 已解 %ld 个成员, 清单 %ld 条 -> %s\n", nmem, x.nreg, ip);

    /* 4) 状态库 */
    {
        char stanza[MAX_STANZA];
        snprintf(stanza, sizeof stanza,
                 "Package: %s\nStatus: install ok installed\n"
                 "Priority: %s\nSection: %s\nInstalled-Size: %s\n"
                 "Maintainer: %s\nArchitecture: %s\nVersion: %s\n"
                 "Depends: %s\nPre-Depends: %s\nConflicts: %s\nMulti-Arch: %s\n"
                 "Description: %s",
                 name, prio ? prio : "optional", sect ? sect : "unknown",
                 size ? size : "0", maint ? maint : "unknown",
                 arch ? arch : "all", version, pend ? pend : "",
                 pre ? pre : "", conf ? conf : "", mver ? mver : "",
                 desc ? desc : "(无描述)");
        if (status_add(stanza) < 0) {
            fprintf(stderr, "dpkg: 状态条目建不起\n");
            close(lockfd);
            goto fail_open;
        }
    }
    if (status_save() < 0) {
        close(lockfd);
        goto fail_open;
    }

    /* 5) postinst */
    info_path(ip, sizeof ip, name, "postinst");
    if (run_script(ip, "configure") < 0)
        fprintf(stderr, "dpkg: %s 的 postinst 失败(文件已落盘, 状态已记)\n", name);

    printf("dpkg: 已安装 %s %s\n", name, version);
    close(lockfd);
    free(name); free(version); free(arch); free(size); free(maint);
    free(prio); free(sect); free(pend); free(pre); free(conf); free(mver);
    free(desc); free(control);
    deb_close(&d);
    return 0;

fail_open:
    free(name); free(version); free(arch); free(size); free(maint);
    free(prio); free(sect); free(pend); free(pre); free(conf); free(mver);
    free(desc); free(control);
    deb_close(&d);
    return 2;
}

/* ------------------------- 卸载 ------------------------- */
static void remove_files(const char *pkg, int *nfile, int *ndir, int *nkeep)
{
    char p[680];
    info_path(p, sizeof p, pkg, "list");
    FILE *f = fopen(p, "r");
    *nfile = *ndir = *nkeep = 0;
    if (!f)
        return;                                /* 没有清单: 没什么可删 */
    char line[1024];
    char **v = NULL;
    int nv = 0, cap = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0])
            continue;
        if (nv == cap) {
            cap = cap ? cap * 2 : 512;
            char **t = realloc(v, (size_t)cap * sizeof *v);
            if (!t)
                break;
            v = t;
        }
        v[nv++] = strdup(line);
    }
    fclose(f);
    /* 清单是安装顺序, 删时要反序: 先删文件, 再删(现在空的)目录。 */
    for (int i = nv - 1; i >= 0; i--) {
        char full[1120];
        snprintf(full, sizeof full, "%s%s", opt_root, v[i]);
        struct stat st;
        if (lstat(full, &st) != 0) {
            free(v[i]);
            continue;
        }
        int ok;
        if (S_ISDIR(st.st_mode)) {
            ok = (rmdir(full) == 0);
            if (ok)
                (*ndir)++;
        } else {
            ok = (unlink(full) == 0);
            if (ok)
                (*nfile)++;
        }
        if (!ok)
            (*nkeep)++;                         /* 非空目录/权限不够 */
        free(v[i]);
    }
    free(v);
}

static int do_remove(const char *spec, int purge)
{
    char pkg[256];
    const char *colon = strchr(spec, ':');
    if (colon)
        snprintf(pkg, sizeof pkg, "%.*s", (int)(colon - spec), spec);
    else
        snprintf(pkg, sizeof pkg, "%s", spec);

    status_load();
    struct pkg *k = pkg_find(pkg);
    if (!k) {
        fprintf(stderr, "dpkg: 包 %s 不在状态库里(没装过?)\n", pkg);
        return 1;
    }
    int lockfd = take_lock();
    if (lockfd < 0)
        return 2;
    char ip[680];
    if (pkg_is_installed(pkg)) {
        info_path(ip, sizeof ip, pkg, "prerm");
        if (run_script(ip, purge ? "remove" : "remove") < 0) {
            fprintf(stderr, "dpkg: %s 的 prerm 失败, 拒绝%s\n", pkg,
                    purge ? "清除" : "卸载");
            close(lockfd);
            return 2;
        }
        int nfile, ndir, nkeep;
        remove_files(pkg, &nfile, &ndir, &nkeep);
        printf("dpkg: 删 %s: %d 个文件", pkg, nfile);
        if (ndir)
            printf(", %d 个空目录", ndir);
        if (nkeep)
            printf(", %d 个条目没删掉(非空目录或权限)", nkeep);
        printf("\n");
        info_path(ip, sizeof ip, pkg, "postrm");
        if (run_script(ip, "remove") < 0)
            fprintf(stderr, "dpkg: %s 的 postrm 失败(文件已删, 状态未改)\n", pkg);
    }
    if (purge) {
        static const char *exts[] = { "list", "md5sums", "conffiles", "control",
                                      "preinst", "postinst", "prerm", "postrm",
                                      "shlibs", "triggers", NULL };
        for (int i = 0; exts[i]; i++) {
            info_path(ip, sizeof ip, pkg, exts[i]);
            unlink(ip);
        }
        pkg_drop(pkg);
    } else {
        char stanza[MAX_STANZA];
        snprintf(stanza, sizeof stanza,
                 "Package: %s\nStatus: deinstall ok config-files\n"
                 "Architecture: %s\nVersion: %s\n",
                 pkg, k->arch ? k->arch : "all", k->version ? k->version : "0");
        if (status_add(stanza) < 0) {
            close(lockfd);
            return 2;
        }
    }
    if (status_save() < 0) {
        close(lockfd);
        return 2;
    }
    printf("dpkg: %s %s\n", pkg, purge ? "已清除(purge)" : "已卸载");
    close(lockfd);
    return 0;
}

/* ------------------------- 查询 ------------------------- */
static void print_stanzas(const char *pat)
{
    status_load();
    printf("Desired=Unknown/Install/Remove/Purge/Hold\n"
           "| Status=Not/Inst/Conf-files/Multi/deInst\n"
           "||/ Name           Version        Architecture Description\n");
    int n = 0;
    for (int i = 0; i < npkg; i++) {
        if (pat && *pat && !strstr(pkgs[i].name, pat))
            continue;
        char *d = field_of(pkgs[i].raw, "Description");
        const char *d1 = d ? d : "";
        const char *nl = strchr(d1, '\n');
        printf("%s %-14s %-14s %-12s %.*s\n",
               !strcmp(pkgs[i].statusword, "installed") ? "ii" : "rc",
               pkgs[i].name, pkgs[i].version ? pkgs[i].version : "-",
               pkgs[i].arch ? pkgs[i].arch : "-",
               nl ? (int)(nl - d1) : (int)strlen(d1), d1);
        free(d);
        n++;
    }
    if (!n)
        printf("(状态库里没有匹配的包)\n");
}

struct listctx {
    int n;
};

static int list_cb(const struct pc_mem *m, void *ud)
{
    struct listctx *lc = (struct listctx *)ud;
    const char *t = m->type == PC_DIR ? "d" : m->type == PC_LNK ? "l"
                  : m->type == PC_HLNK ? "h" : m->type == PC_CHR ? "c"
                  : m->type == PC_BLK ? "b" : m->type == PC_FIFO ? "p" : "-";
    printf("%s%04o %s", t, m->mode & 07777, m->name);
    if (m->type == PC_LNK)
        printf(" -> %s", m->link);
    else if (m->type == PC_HLNK)
        printf(" link to %s", m->link);
    else if (m->type == PC_REG)
        printf(" %ld", m->size);
    printf("\n");
    lc->n++;
    return 0;
}

static int query_control_field(const char *path, const char *fld)
{
    struct deb d;
    if (deb_open(&d, path) < 0)
        return 2;
    char *control = ctrl_file(&d, "control");
    int rc = 0;
    if (!control) {
        fprintf(stderr, "dpkg: %s 里没有 control\n", path);
        rc = 2;
    } else if (fld) {
        char *v = field_of(control, fld);
        if (v)
            printf("%s\n", v);
        else
            rc = 1;
        free(v);
    } else {
        char *desc = field_of(control, "Description");
        printf(" new Debian package, version 2.0.\n");
        printf(" size %ld bytes: control archive=%s\n", d.len, d.ctrlname);
        for (char *p = control; *p;) {
            char *nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            if (ll && p[0] != ' ' && p[0] != '\t' && memchr(p, ':', ll))
                printf("%.*s\n", (int)ll, p);
            if (!nl)
                break;
            p = nl + 1;
        }
        if (desc)
            printf(" Description: %s\n", desc);
    }
    free(control);
    deb_close(&d);
    return rc;
}

static void usage(void)
{
    printf("dpkg -Parlz/" DEB_VERSION " (Debian .deb 底层安装器, 移植实现)\n");
    printf("用法: dpkg [选项] 动作\n");
    printf("  -i|--install <包.deb>...        安装/升级\n");
    printf("  -r|--remove <包>...             卸载(conffiles 留着)\n");
    printf("  -P|--purge <包>...              卸载并清配置与状态条目\n");
    printf("  -l|--list [模式]                列出状态库里的包\n");
    printf("  -s|--status <包>                打印状态条目\n");
    printf("  -L|--listfiles <包>             打印已装文件清单\n");
    printf("  -c|--contents <包.deb>          打印包里 data 的成员表\n");
    printf("  -I|--info <包.deb> [字段]       打印 control 字段(或单个字段值)\n");
    printf("  --compare-versions <a> <op> <b> Debian 版本比较(真=0 假=1)\n");
    printf("选项: --root <目录> --instdir <目录> --admindir <目录>\n");
    printf("      --force-architecture --force-downgrade --force-overwrite\n");
    printf("      --force-all --no-scripts -v|--version\n");
}

int main(int argc, char **argv)
{
    const char *act = NULL;
    char *args[64];
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("dpkg -Parlz/" DEB_VERSION " (基于 pkgcore 的移植实现)\n");
            printf("  支持的包: debian-binary=2.0 的 ar 档"
                   "(control.tar[.gz] + data.tar[.gz])\n");
            printf("  本系统架构: %s\n", host_arch());
            return 0;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(a, "--root") || !strcmp(a, "--instdir") ||
            !strcmp(a, "--admindir")) {
            if (++i >= argc) {
                fprintf(stderr, "dpkg: %s 需要参数\n", a);
                return 2;
            }
            char *dst = (!strcmp(a, "--admindir")) ? opt_adm : opt_root;
            size_t dn = (!strcmp(a, "--admindir")) ? sizeof opt_adm
                                                   : sizeof opt_root;
            snprintf(dst, dn, "%s", argv[i]);
            char *sl = dst + strlen(dst) - 1;
            while (sl > dst && *sl == '/')
                *sl-- = 0;
            continue;
        }
        if (!strcmp(a, "--force-architecture")) { force_arch = 1; continue; }
        if (!strcmp(a, "--force-overwrite")) { force_overwrite = 1; continue; }
        if (!strcmp(a, "--force-downgrade")) { force_overwrite = 1; continue; }
        if (!strcmp(a, "--force-all")) { force_all = 1; continue; }
        if (!strcmp(a, "--no-scripts")) { no_scripts = 1; continue; }
        if (!strcmp(a, "--no-debsig") || !strcmp(a, "--auto-deconfigure") ||
            !strcmp(a, "--no-triggers")) continue;      /* 本实现没有这些机制 */
        if (!strcmp(a, "--compare-versions")) { act = "compare"; continue; }
        if (!strcmp(a, "-i") || !strcmp(a, "--install")) { act = "install"; continue; }
        if (!strcmp(a, "-r") || !strcmp(a, "--remove")) { act = "remove"; continue; }
        if (!strcmp(a, "-P") || !strcmp(a, "--purge")) { act = "purge"; continue; }
        if (!strcmp(a, "-l") || !strcmp(a, "--list")) { act = "list"; continue; }
        if (!strcmp(a, "-s") || !strcmp(a, "--status")) { act = "status"; continue; }
        if (!strcmp(a, "-L") || !strcmp(a, "--listfiles")) { act = "listfiles"; continue; }
        if (!strcmp(a, "-c") || !strcmp(a, "--contents")) { act = "contents"; continue; }
        if (!strcmp(a, "-I") || !strcmp(a, "--info") ||
            !strcmp(a, "--field")) { act = "info"; continue; }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "dpkg: 不认的选项 %s(见 dpkg --help)\n", a);
            return 2;
        }
        if (nargs < 64)
            args[nargs++] = a;
    }
    paths_init();

    if (!act) {
        usage();
        return 2;
    }
    if (!strcmp(act, "compare")) {
        if (nargs != 3) {
            fprintf(stderr, "dpkg: --compare-versions 需要 3 个参数\n");
            return 2;
        }
        return pc_deb_ver_match(args[0], args[1], args[2]) ? 0 : 1;
    }
    if (!strcmp(act, "install")) {
        if (!nargs) {
            fprintf(stderr, "dpkg: install 需要 .deb 文件路径\n");
            return 2;
        }
        int rc = 0;
        for (int i = 0; i < nargs; i++)
            if (do_install(args[i]) && !rc)
                rc = 2;
        return rc;
    }
    if (!strcmp(act, "remove") || !strcmp(act, "purge")) {
        if (!nargs) {
            fprintf(stderr, "dpkg: %s 需要包名\n", act);
            return 2;
        }
        int rc = 0;
        for (int i = 0; i < nargs; i++)
            if (do_remove(args[i], !strcmp(act, "purge")) && !rc)
                rc = 1;
        return rc;
    }
    if (!strcmp(act, "list")) {
        print_stanzas(nargs ? args[0] : NULL);
        return 0;
    }
    if (!strcmp(act, "status")) {
        if (!nargs) {
            fprintf(stderr, "dpkg: status 需要包名\n");
            return 2;
        }
        status_load();
        struct pkg *k = pkg_find(args[0]);
        if (!k) {
            printf("dpkg-query: package '%s' is not installed\n", args[0]);
            return 1;
        }
        printf("%s\n\n", k->raw);
        return 0;
    }
    if (!strcmp(act, "listfiles")) {
        if (!nargs) {
            fprintf(stderr, "dpkg: listfiles 需要包名\n");
            return 2;
        }
        char p[680];
        info_path(p, sizeof p, args[0], "list");
        FILE *f = fopen(p, "r");
        if (!f) {
            fprintf(stderr, "dpkg: %s 没有清单(没装过?)\n", args[0]);
            return 1;
        }
        char line[1024];
        int n = 0;
        while (fgets(line, sizeof line, f)) {
            fputs(line, stdout);
            n++;
        }
        fclose(f);
        return n ? 0 : 1;
    }
    if (!strcmp(act, "contents")) {
        if (!nargs) {
            fprintf(stderr, "dpkg: contents 需要 .deb 路径\n");
            return 2;
        }
        struct deb d;
        if (deb_open(&d, args[0]) < 0)
            return 2;
        struct listctx lc;
        memset(&lc, 0, sizeof lc);
        pc_tar_walk(d.data, d.datalen, list_cb, &lc);
        printf("(%s: %d 个成员)\n", d.dataname, lc.n);
        deb_close(&d);
        return lc.n ? 0 : 2;
    }
    if (!strcmp(act, "info")) {
        if (!nargs) {
            fprintf(stderr, "dpkg: info 需要 .deb 路径\n");
            return 2;
        }
        return query_control_field(args[0], nargs > 1 ? args[1] : NULL);
    }
    fprintf(stderr, "dpkg: 未实现的动作 %s\n", act);
    return 2;
}
