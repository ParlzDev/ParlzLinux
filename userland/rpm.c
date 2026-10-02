// SPDX-License-Identifier: GPL-2.0-or-later
/* rpm.c - Parlz 移植的 rpm(.rpm 包的底层安装器)。
 *
 * 包格式(.rpm): 96 字节 lead(魔数 ed ab ee db) + 签名 header + 主 header
 *   + 负载(newc cpio, 默认 gzip)。header 每段 16 字节:
 *   magic(4) + reserved(4) + 条目数(4) + 值区长度(4); 值区紧跟索引之后,
 *   索引项里的 offset 相对值区起点(整数数组还要按类型对齐)。
 *   标签号/类型不靠记忆: 用 scripts/rpmhdr.py 在真 rpmbuild 产出的包上核过 ——
 *   1000 NAME / 1001 VERSION / 1002 RELEASE / 1004 SUMMARY / 1005 DESCRIPTION /
 *   1009 SIZE / 1014 LICENSE / 1022 ARCH / 1023..1026 PREIN POSTIN PREUN POSTUN /
 *   1085..1088 对应的解释器 / 1116..1118 文件路径三件套 / 1124..1126 负载格式。
 * 状态库(纯文本, 不是上游的 bdb/sqlite):
 *   <dbpath=/var/lib/rpm>/installed/<NVRA>.meta  字段行
 *   <dbpath>/installed/<NVRA>.list               已落盘路径(卸载按它删)
 *   文件清单直接取负载 cpio 的成员名 —— 它才是"包里到底有什么"的权威。
 *
 * 子命令:
 *   -i|--install <file.rpm>...     安装(脚本 → 抽 cpio → 登记)
 *   -U|--upgrade <file.rpm>...     升级(装新的, 再删旧的里多出来的文件)
 *   -e|--erase <包>...             卸载
 *   -q <包> / -qa / -qi <包> / -ql <包> / -qf <路径>       查已装
 *   -qp <file> / -qpi <file> / -qpl <file>                 查包文件
 *   --compare-versions <a> <op> <b>  RPM vercmp(真=0)
 *   --root <目录> / --dbpath <目录>(也认 RPM_ROOT 环境变量)
 *
 * 与上游 rpm 的已知差别(不冒充全量实现):
 *   - 依赖只做到"能不能查", 求解在 yum 那一层; 这里 --nodeps 是默认行为。
 *   - 负载只支持 cpio + gzip(xz/zst 明确报错); 不验 GPG/PGP 签名。
 *   - 没有 %trigger、没有 alternatives、没有事务脚本回滚。
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
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <sys/utsname.h>

#include "pkgcore.h"

#define RPM_PORT_VERSION "1.0.0"
#define SCRIPT_TIMEOUT 300

/* header 标签(实测核对, 见文件头注释) */
#define T_NAME      1000
#define T_VERSION   1001
#define T_RELEASE   1002
#define T_SUMMARY   1004
#define T_DESCR     1005
#define T_SIZE      1009
#define T_LICENSE   1014
#define T_ARCH      1022
#define T_PREIN     1023
#define T_POSTIN    1024
#define T_PREUN     1025
#define T_POSTUN    1026
#define T_FILEMODES 1030
#define T_PREINPROG  1085
#define T_POSTINPROG 1086
#define T_PREUNPROG  1087
#define T_POSTUNPROG 1088
#define T_DIRINDEX  1116
#define T_BASENAME  1117
#define T_DIRNAME   1118
#define T_PLGFMT    1124
#define T_PLGCOMP   1125
#define T_PLGFLAGS  1126
#define T_HDRSHA1   5097

/* header 数据类型 */
#define DT_CHAR 1
#define DT_I8   2
#define DT_I16  3
#define DT_I32  4
#define DT_I64  5
#define DT_STR  6
#define DT_BIN  7
#define DT_STRA 8                 /* 串数组: count 是**字节数** */
#define DT_I18N 9                 /* i18n 串数组: count 是串数 */

static char opt_root[512];
static char opt_db[512];
static int  opt_noscripts, opt_force, opt_replace, opt_oldpkg;

static void paths_init(void)
{
    const char *e;
    if (!opt_root[0]) {
        e = getenv("RPM_ROOT");
        if (e && *e)
            snprintf(opt_root, sizeof opt_root, "%s", e);
    }
    if (!opt_db[0])
        snprintf(opt_db, sizeof opt_db, "%s/var/lib/rpm", opt_root);
}

static int take_lock(void)
{
    char p[600];
    pc_mkdirs(opt_db, 0755);
    snprintf(p, sizeof p, "%s/lock", opt_db);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    for (int i = 0; i < 120; i++) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            return fd;
        if (i == 0)
            fprintf(stderr, "rpm: 另一个 rpm/yum 正在装包, 等锁...\n");
        usleep(500000);
    }
    fprintf(stderr, "rpm: 取不到 %s 的锁\n", p);
    close(fd);
    return -1;
}

static const char *host_arch(void)
{
    static char a[64];
    if (a[0])
        return a;
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(a, sizeof a, "%s", u.machine);
    else
        snprintf(a, sizeof a, "x86_64");
    return a;
}

/* ------------------------- header 解析 ------------------------- */
struct hdr {
    const unsigned char *d;
    long len;
    long store;                    /* 主 header 值区起点 */
    long nent;
    const unsigned char *pl;       /* 负载起点 */
    long pllen;
    char plfmt[32], plcomp[32];
};

static unsigned be32(const unsigned char *p)
{
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) |
           ((unsigned)p[2] << 8) | (unsigned)p[3];
}

static long sec_align8(long o)
{
    return o + ((8 - (o % 8)) % 8);
}

/* 定位主 header: lead(96) 后第一段的值区之后, 再按 8 对齐就是第二段。 */
static int hdr_open(struct hdr *h, const unsigned char *d, long len)
{
    h->d = d;
    h->len = len;
    h->store = h->nent = 0;
    h->pl = NULL;
    h->pllen = 0;
    h->plfmt[0] = h->plcomp[0] = 0;
    if (len < 96 + 16)
        return -1;
    if (!(d[0] == 0xed && d[1] == 0xab && d[2] == 0xee && d[3] == 0xdb)) {
        fprintf(stderr, "rpm: 不是 .rpm 文件(lead 魔数是 %02x%02x%02x%02x,"
                        " 应为 edabee db)\n", d[0], d[1], d[2], d[3]);
        return -1;
    }
    long off = 96;
    for (int sec = 0; sec < 2; sec++) {
        if (off + 16 > len)
            return -1;
        if (!(d[off] == 0x8e && d[off + 1] == 0xad && d[off + 2] == 0xe8)) {
            fprintf(stderr, "rpm: 偏移 %ld 处没有 header 魔数 8e ad e8\n", off);
            return -1;
        }
        long nent = (long)be32(d + off + 8);
        long dlen = (long)(int)be32(d + off + 12);
        long store = off + 16 + nent * 16;
        long end = store + dlen;
        if (nent < 0 || dlen < 0 || end > len) {
            fprintf(stderr, "rpm: header 段长度不合法(条目=%ld 值区=%ld)\n",
                    nent, dlen);
            return -1;
        }
        if (sec == 1) {
            h->store = store;
            h->nent = nent;
            /* 只有"签名段 → 主 header 段"之间要 8 字节对齐;
             * 负载**紧跟**主 header 值区末尾, 不补齐(实测 gzip 魔数就在 end)。
             * 在这里多补一次齐, 就会把 gzip 流的头 3 字节切掉,
             * 报出"负载标称 gzip 但没有 gzip 魔数"这种看着像文件损坏的错。 */
            h->pl = d + end;
            h->pllen = len - end;
        }
        off = sec_align8(end);
    }
    if (!h->nent)
        return -1;
    return 0;
}

/* 取一个标签的原始索引项 */
static int hdr_ent(const struct hdr *h, long tag, long *typ, long *dat,
                   long *cnt)
{
    for (long i = 0; i < h->nent; i++) {
        /* 索引项在主 header 段里: 段起点 = store - 16 - nent*16 */
        long start = h->store - 16 - h->nent * 16;
        const unsigned char *p = h->d + start + 16 + i * 16;
        if ((long)be32(p) == tag) {
            *typ = (long)be32(p + 4);
            *dat = (long)(int)be32(p + 8);
            *cnt = (long)(int)be32(p + 12);
            return 0;
        }
    }
    return -1;
}

static int type_align(long t)
{
    if (t == DT_I16)
        return 2;
    if (t == DT_I32)
        return 4;
    if (t == DT_I64)
        return 8;
    return 1;
}

/* 串数组类标签 -> 第 idx 个串(找不到返回 NULL)。
 * DT_STRA(8) 的 count 是**字节数**, 不是一个一个的串: 按字节区里的 NUL 切,
 * 取第 idx 个(这点上踩过: 按 count 次循环只捞到 1 个名字)。 */
static char *hdr_strn(const struct hdr *h, long tag, int idx)
{
    long typ, dat, cnt;
    if (hdr_ent(h, tag, &typ, &dat, &cnt) < 0)
        return NULL;
    long a = type_align(typ);
    long o = h->store + dat + ((a > 1) ? ((-dat) % a) : 0);
    if (o < 0 || o > h->len)
        return NULL;
    if (typ == DT_STR) {
        if (idx != 0)
            return NULL;
        long e = o;
        while (e < h->len && h->d[e])
            e++;
        char *s = malloc((size_t)(e - o) + 1);
        if (s) {
            memcpy(s, h->d + o, (size_t)(e - o));
            s[e - o] = 0;
        }
        return s;
    }
    if (typ == DT_STRA || typ == DT_I18N) {
        long bytes = (typ == DT_STRA) ? cnt : 0;
        long lim = (typ == DT_STRA) ? o + bytes : h->len;
        long q = o, k = 0;
        while (q < lim) {
            long e = q;
            while (e < h->len && h->d[e])
                e++;
            if (k == idx) {
                char *s = malloc((size_t)(e - q) + 1);
                if (s) {
                    memcpy(s, h->d + q, (size_t)(e - q));
                    s[e - q] = 0;
                }
                return s;
            }
            k++;
            q = e + 1;
            if (typ == DT_I18N && k >= cnt)
                break;
        }
        return NULL;
    }
    return NULL;
}

static char *hdr_str(const struct hdr *h, long tag)
{
    return hdr_strn(h, tag, 0);
}

/* 整数类标签 -> 第 idx 个值; *ok 置失败标志 */
long hdr_num(const struct hdr *h, long tag, int idx, int *ok)
{
    long typ, dat, cnt;
    if (ok)
        *ok = 0;
    if (hdr_ent(h, tag, &typ, &dat, &cnt) < 0)
        return 0;
    long a = type_align(typ);
    long o = h->store + dat + ((a > 1) ? ((-dat) % a) : 0);
    if (idx < 0 || (long)cnt <= idx)
        return 0;
    if (typ == DT_I32 && o + 4 * (idx + 1) <= h->len) {
        if (ok)
            *ok = 1;
        return (long)(int)be32(h->d + o + 4 * idx);
    }
    if (typ == DT_I16 && o + 2 * (idx + 1) <= h->len) {
        int v = (int)be32(h->d + o + 2 * idx) & 0xffff;
        if (ok)
            *ok = 1;
        return (v & 0x8000) ? v - 0x10000 : v;
    }
    if (typ == DT_I8 && o + idx < h->len) {
        if (ok)
            *ok = 1;
        return (long)(signed char)h->d[o + idx];
    }
    if (typ == DT_CHAR && o + idx < h->len) {
        if (ok)
            *ok = 1;
        return (long)h->d[o + idx];
    }
    return 0;
}

static long hdr_count(const struct hdr *h, long tag)
{
    long typ, dat, cnt;
    if (hdr_ent(h, tag, &typ, &dat, &cnt) < 0)
        return 0;
    if (typ == DT_STRA) {
        /* count 是字节数 -> 数 NUL 得到串个数 */
        long a = 1;
        long o = h->store + dat + ((a > 1) ? ((-dat) % a) : 0);
        long n = 0;
        for (long i = 0; i < cnt && o + i < h->len; i++)
            if (h->d[o + i] == 0)
                n++;
        return n;
    }
    if (typ == DT_STR)
        return 1;
    return cnt;
}

/* ------------------------- 负载(cpio) ------------------------- */
static int payload_get(struct hdr *h, unsigned char **out, long *olen,
                       int *owned)
{
    *out = NULL;
    *olen = 0;
    *owned = 0;
    if (!h->pl || h->pllen <= 0) {
        fprintf(stderr, "rpm: 文件里没有负载\n");
        return -1;
    }
    char *fmt = hdr_str(h, T_PLGFMT);
    char *cmp = hdr_str(h, T_PLGCOMP);
    if (fmt && strcmp(fmt, "cpio")) {
        fprintf(stderr, "rpm: 负载格式是 %s, 本实现只支持 cpio(newc)\n", fmt);
        free(fmt);
        free(cmp);
        return -1;
    }
    snprintf(h->plfmt, sizeof h->plfmt, "%s", fmt ? fmt : "cpio");
    snprintf(h->plcomp, sizeof h->plcomp, "%s", cmp ? cmp : "?");
    free(fmt);
    free(cmp);
    const unsigned char *p = h->pl;
    long pl = h->pllen;
    if (p[0] == 0x1f && p[1] == 0x8b) {           /* gzip */
        if (pc_gunzip(p, pl, out, olen) != 0) {
            fprintf(stderr, "rpm: 解 gzip 负载失败(流损坏?)\n");
            return -1;
        }
        *owned = 1;
        return 0;
    }
    if (!strncmp(h->plcomp, "gzip", 4) && !(p[0] == '0' && p[1] == '7')) {
        fprintf(stderr, "rpm: 负载标称 gzip 但没有 gzip 魔数(文件被截断?)\n");
        return -1;
    }
    if (!(p[0] == '0' && p[1] == '7')) {
        fprintf(stderr, "rpm: 负载既不是 gzip 也不是明文 cpio(压缩器=%s);"
                        " 本实现只支持 gzip 与不压缩, xz/zst 请换 cpio 压缩器\n",
                h->plcomp);
        return -1;
    }
    *out = (unsigned char *)p;
    *olen = pl;
    return 0;
}

/* ------------------------- 元信息 ------------------------- */
struct meta {
    char name[256], version[128], release[128], arch[128];
    char summary[512], license[128], descr[768];
    char preunprog[160], postunprog[160];   /* 卸载时要用的解释器(通常 /bin/sh) */
    long installsize;
};

static void meta_from_hdr(struct meta *m, const struct hdr *h)
{
    char *s;
    memset(m, 0, sizeof *m);
    if ((s = hdr_str(h, T_NAME))) {
        snprintf(m->name, sizeof m->name, "%s", s);
        free(s);
    }
    if ((s = hdr_str(h, T_VERSION))) {
        snprintf(m->version, sizeof m->version, "%s", s);
        free(s);
    }
    if ((s = hdr_str(h, T_RELEASE))) {
        snprintf(m->release, sizeof m->release, "%s", s);
        free(s);
    }
    if ((s = hdr_str(h, T_ARCH))) {
        snprintf(m->arch, sizeof m->arch, "%s", s);
        free(s);
    }
    if ((s = hdr_str(h, T_SUMMARY))) {
        snprintf(m->summary, sizeof m->summary, "%s", s);
        free(s);
    }
    if ((s = hdr_str(h, T_LICENSE))) {
        snprintf(m->license, sizeof m->license, "%s", s);
        free(s);
    }
    if ((s = hdr_str(h, T_DESCR))) {
        snprintf(m->descr, sizeof m->descr, "%s", s);
        free(s);
    }
    int ok = 0;
    m->installsize = hdr_num(h, T_SIZE, 0, &ok);
    if (!ok)
        m->installsize = 0;
    /* 卸载脚本要在**没有包文件**的时候也能跑(那时只有状态库),
     * 所以解释器路径必须跟着状态库存一份。 */
    if ((s = hdr_str(h, T_PREUNPROG))) {
        snprintf(m->preunprog, sizeof m->preunprog, "%s", s);
        free(s);
    }
    if ((s = hdr_str(h, T_POSTUNPROG))) {
        snprintf(m->postunprog, sizeof m->postunprog, "%s", s);
        free(s);
    }
}

static void nvra(const struct meta *m, char *out, size_t n)
{
    snprintf(out, n, "%s-%s-%s.%s", m->name, m->version, m->release, m->arch);
}

/* ------------------------- 状态库(纯文本) ------------------------- */
struct dbent {
    char nvra[640];
    struct meta m;
};

static void db_dir(char *out, size_t n)
{
    snprintf(out, n, "%s/installed", opt_db);
    pc_mkdirs(out, 0755);
}

static void db_meta_path(char *out, size_t n, const char *nvra_s)
{
    char d[600];
    db_dir(d, sizeof d);
    snprintf(out, n, "%s/%s.meta", d, nvra_s);
}

static void db_list_path(char *out, size_t n, const char *nvra_s)
{
    char d[600];
    db_dir(d, sizeof d);
    snprintf(out, n, "%s/%s.list", d, nvra_s);
}

static int db_load(struct dbent *ents, int max)
{
    char d[600];
    db_dir(d, sizeof d);
    DIR *dp = opendir(d);
    int n = 0;
    if (!dp)
        return 0;
    struct dirent *e;
    while ((e = readdir(dp)) && n < max) {
        char *dot = strstr(e->d_name, ".meta");
        if (!dot)
            continue;
        char p[700];
        snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        char line[1024];
        struct dbent *k = &ents[n];
        memset(k, 0, sizeof *k);
        snprintf(k->nvra, sizeof k->nvra, "%.*s", (int)(dot - e->d_name),
                 e->d_name);
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *v = strchr(line, '=');
            if (!v)
                continue;
            *v++ = 0;
            if (!strcmp(line, "name"))
                snprintf(k->m.name, sizeof k->m.name, "%s", v);
            else if (!strcmp(line, "version"))
                snprintf(k->m.version, sizeof k->m.version, "%s", v);
            else if (!strcmp(line, "release"))
                snprintf(k->m.release, sizeof k->m.release, "%s", v);
            else if (!strcmp(line, "arch"))
                snprintf(k->m.arch, sizeof k->m.arch, "%s", v);
            else if (!strcmp(line, "summary"))
                snprintf(k->m.summary, sizeof k->m.summary, "%s", v);
            else if (!strcmp(line, "license"))
                snprintf(k->m.license, sizeof k->m.license, "%s", v);
            else if (!strcmp(line, "size"))
                k->m.installsize = strtol(v, NULL, 10);
            else if (!strcmp(line, "preunprog"))
                snprintf(k->m.preunprog, sizeof k->m.preunprog, "%s", v);
            else if (!strcmp(line, "postunprog"))
                snprintf(k->m.postunprog, sizeof k->m.postunprog, "%s", v);
        }
        fclose(f);
        if (k->m.name[0])
            n++;
        else
            memset(k, 0, sizeof *k);
    }
    closedir(dp);
    return n;
}

static int db_write(const struct meta *m)
{
    char nv[640], p[700];
    nvra(m, nv, sizeof nv);
    db_meta_path(p, sizeof p, nv);
    char t[740];
    snprintf(t, sizeof t, "%s.new", p);
    FILE *f = fopen(t, "w");
    if (!f) {
        fprintf(stderr, "rpm: 写 %s 失败: %s\n", t, strerror(errno));
        return -1;
    }
    fprintf(f, "name=%s\nversion=%s\nrelease=%s\narch=%s\nsize=%ld\n",
            m->name, m->version, m->release, m->arch, m->installsize);
    fprintf(f, "summary=%s\nlicense=%s\n", m->summary, m->license);
    fprintf(f, "preunprog=%s\npostunprog=%s\n", m->preunprog, m->postunprog);
    fprintf(f, "installtime=%ld\n", (long)time(NULL));
    int ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    fclose(f);
    if (!ok || rename(t, p) != 0) {
        fprintf(stderr, "rpm: 落 %s 失败: %s\n", p, strerror(errno));
        unlink(t);
        return -1;
    }
    sync();
    return 0;
}

static const struct dbent *db_find(struct dbent *ents, int n, const char *spec)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(ents[i].m.name, spec))
            return &ents[i];
        if (!strcmp(ents[i].nvra, spec))
            return &ents[i];
        char nr[640];
        snprintf(nr, sizeof nr, "%s-%s", ents[i].m.name, ents[i].m.version);
        if (!strcmp(nr, spec))
            return &ents[i];
        /* -e pkgdemo-1.2.3-4 这种"名字-版本-发布号"的写法也要认得
         * (两版本并存时只能用它点准一条) */
        snprintf(nr, sizeof nr, "%s-%s-%s", ents[i].m.name, ents[i].m.version,
                 ents[i].m.release);
        if (!strcmp(nr, spec))
            return &ents[i];
    }
    return NULL;
}

/* ------------------------- 脚本 ------------------------- */
static void scr_path(char *out, size_t n, const char *nvra_s, const char *which)
{
    char d[600];
    snprintf(d, sizeof d, "%s/scriptlets", opt_db);
    pc_mkdirs(d, 0700);
    snprintf(out, n, "%s/%s.%s", d, nvra_s, which);
}

/* 按路径跑一个脚本段。文件不存在 = 这个包没写这段脚本, 返回 0。 */
static int run_script_path(const char *p, const char *prog, const char *arg)
{
    struct stat st;
    if (stat(p, &st) != 0)
        return 0;
    if (opt_noscripts) {
        printf("rpm: 跳过脚本段 %s(--noscripts)\n", pc_basename(p));
        return 0;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "rpm: fork 失败: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        char sh[200];
        snprintf(sh, sizeof sh, "%s", (prog && *prog) ? prog : "/bin/sh");
        if (opt_root[0])
            setenv("RPM_ROOT", opt_root, 1);
        setenv("RPM_DBPATH", opt_db, 1);
        execl(sh, sh, p, arg, (char *)NULL);
        fprintf(stderr, "rpm: 跑 %s 需要解释器 %s, execv 失败: %s\n",
                p, sh, strerror(errno));
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
            fprintf(stderr, "rpm: 脚本 %s 超过 %d 秒, 杀掉它(不静默长等)\n",
                    pc_basename(p), SCRIPT_TIMEOUT);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        if (waited % 30 == 0)
            fprintf(stderr, "rpm: 脚本 %s %s 已跑 %ld 秒...\n",
                    pc_basename(p), arg, waited);
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
    fprintf(stderr, "rpm: 脚本 %s %s 失败(%s%d)\n", pc_basename(p), arg,
            WIFSIGNALED(status) ? "死于信号 " : "退出码 ",
            WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
    return -1;
}

/* ------------------------- 抽取 + 清单 ------------------------- */
struct xctx {
    struct pc_extract *e;
    char **paths;
    long n, cap;
};

static int add_path(struct xctx *x, const char *p)
{
    if (x->n == x->cap) {
        long nc = x->cap ? x->cap * 2 : 256;
        char **t = realloc(x->paths, (size_t)nc * sizeof *t);
        if (!t)
            return -1;
        x->paths = t;
        x->cap = nc;
    }
    x->paths[x->n] = strdup(p);
    if (!x->paths[x->n])
        return -1;
    x->n++;
    return 0;
}

static int x_cb(const struct pc_mem *m, void *ud)
{
    struct xctx *x = (struct xctx *)ud;
    if (pc_extract_mem(x->e, m) != 0)
        return -1;
    char rel[1100];
    if (pc_member_path("", m->name, rel, sizeof rel) == 0 && rel[0] &&
        strcmp(rel, "/"))
        add_path(x, rel);
    return 0;
}

static int write_list(const struct meta *m, struct xctx *x)
{
    char nv[640], p[700];
    nvra(m, nv, sizeof nv);
    db_list_path(p, sizeof p, nv);
    unlink(p);
    FILE *f = fopen(p, "w");
    if (!f) {
        fprintf(stderr, "rpm: 写 %s 失败: %s\n", p, strerror(errno));
        return -1;
    }
    for (long i = 0; i < x->n; i++)
        fprintf(f, "%s\n", x->paths[i]);
    int ok = (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    fclose(f);
    if (!ok) {
        fprintf(stderr, "rpm: 刷 %s 失败\n", p);
        return -1;
    }
    return 0;
}

/* ------------------------- 安装/升级 ------------------------- */
static int do_install(const char *path, int upgrade)
{
    long len;
    unsigned char *img = pc_slurp(path, &len);
    if (!img) {
        fprintf(stderr, "rpm: 读 %s 失败: %s\n", path, strerror(errno));
        return 1;
    }
    struct hdr h;
    if (hdr_open(&h, img, len) < 0) {
        free(img);
        return 1;
    }
    struct meta m;
    meta_from_hdr(&m, &h);
    if (!m.name[0] || !m.version[0] || !m.release[0]) {
        fprintf(stderr, "rpm: %s 的 header 缺 NAME/VERSION/RELEASE\n", path);
        free(img);
        return 1;
    }
    if (m.arch[0] && strcmp(m.arch, "noarch") && strcmp(m.arch, host_arch()) &&
        !opt_force) {
        fprintf(stderr, "rpm: 包 %s 是 %s 架构, 本机是 %s —— 拒绝安装"
                        "(强行装加 --force)\n", m.name, m.arch, host_arch());
        free(img);
        return 1;
    }
    unsigned char *pl = NULL;
    long pllen = 0;
    int owned = 0;
    if (payload_get(&h, &pl, &pllen, &owned) < 0) {
        free(img);
        return 1;
    }
    int lockfd = take_lock();
    if (lockfd < 0) {
        if (owned)
            free(pl);
        free(img);
        return 1;
    }
    struct dbent *ents = calloc(512, sizeof *ents);
    int nent = ents ? db_load(ents, 512) : 0;
    const struct dbent *old = ents ? db_find(ents, nent, m.name) : NULL;
    if (old) {
        char oa[640], ob[640];
        snprintf(oa, sizeof oa, "%s-%s", old->m.version, old->m.release);
        snprintf(ob, sizeof ob, "%s-%s", m.version, m.release);
        int c = pc_rpm_vercmp(oa, ob);
        if (c > 0 && !opt_oldpkg && !opt_force) {
            fprintf(stderr, "rpm: 已装 %s %s 比要装的 %s 更新 —— 降级要 --oldpackage\n",
                    m.name, oa, ob);
            close(lockfd);
            free(ents);
            if (owned)
                free(pl);
            free(img);
            return 1;
        }
        if (c == 0 && !upgrade && !opt_replace && !opt_force) {
            fprintf(stderr, "rpm: 包 %s 已经装过(%s-%s.%s);"
                            " 重装要 --replacepkgs\n", m.name, m.version,
                    m.release, m.arch);
            close(lockfd);
            free(ents);
            if (owned)
                free(pl);
            free(img);
            return 1;
        }
    }
    printf("rpm: 准备安装 %s-%s-%s.%s(负载 %s+%s, %ld 字节)\n",
           m.name, m.version, m.release, m.arch, h.plfmt, h.plcomp, pllen);

    char nv[640];
    nvra(&m, nv, sizeof nv);
    char arg[8];
    snprintf(arg, sizeof arg, "%d", old ? 2 : 1);     /* 2=升级, 1=全新安装 */

    /* 四段脚本先落库再从库里跑: 卸载时包文件已经不在手上了,
     * %preun/%postun 只能靠 <db>/scriptlets/<NVRA>.* 找回来。 */
    static const long sctag[4] = { T_PREIN, T_POSTIN, T_PREUN, T_POSTUN };
    static const char *const scname[4] = { "prein", "postin", "preun", "postun" };
    static const long scprog[4] = { T_PREINPROG, T_POSTINPROG,
                                    T_PREUNPROG, T_POSTUNPROG };
    char spath[4][700];
    char sprog[4][160];
    for (int i = 0; i < 4; i++) {
        scr_path(spath[i], sizeof spath[i], nv, scname[i]);
        snprintf(sprog[i], sizeof sprog[i], "%s", "/bin/sh");
        char *pr = hdr_str(&h, scprog[i]);
        if (pr && *pr)
            snprintf(sprog[i], sizeof sprog[i], "%s", pr);
        free(pr);
        char *body = hdr_str(&h, sctag[i]);
        if (body && body[0]) {
            if (pc_write_file(spath[i], (const unsigned char *)body,
                              (long)strlen(body), 0700) < 0)
                fprintf(stderr, "rpm: 存脚本段 %s 失败\n", scname[i]);
        } else {
            unlink(spath[i]);
        }
        free(body);
    }
    /* 卸载要用到的两条解释器路径记进 .meta(db_write 在解包之后) */
    snprintf(m.preunprog, sizeof m.preunprog, "%s", sprog[2]);
    snprintf(m.postunprog, sizeof m.postunprog, "%s", sprog[3]);

    if (run_script_path(spath[0], sprog[0], arg) < 0) {
        fprintf(stderr, "rpm: %s 的 %%pre 失败, 中止(一个文件都没解)\n", m.name);
        for (int i = 0; i < 4; i++)
            unlink(spath[i]);
        close(lockfd);
        free(ents);
        if (owned)
            free(pl);
        free(img);
        return 1;
    }

    struct pc_extract *e;
    struct xctx x;
    memset(&x, 0, sizeof x);
    if (pc_extract_init(&e, opt_root) < 0) {
        close(lockfd);
        free(ents);
        if (owned)
            free(pl);
        free(img);
        return 1;
    }
    x.e = e;
    int wr = pc_cpio_walk(pl, pllen, x_cb, &x);
    long nmem = pc_extract_count(e);
    int nerr = pc_extract_errors(e);
    pc_extract_free(e);
    if (wr < 0 || nerr || nmem == 0) {
        fprintf(stderr, "rpm: 解 cpio 负载失败: 成员=%ld, 失败=%d, rc=%d\n",
                nmem, nerr, wr);
        close(lockfd);
        free(ents);
        if (owned)
            free(pl);
        free(img);
        return 1;
    }
    printf("rpm: 已解 %ld 个成员, 登记清单 %ld 条\n", nmem, x.n);
    if (write_list(&m, &x) < 0 || db_write(&m) < 0) {
        close(lockfd);
        for (long i = 0; i < x.n; i++)
            free(x.paths[i]);
        free(x.paths);
        free(ents);
        if (owned)
            free(pl);
        free(img);
        return 1;
    }

    /* 升级: 旧包里多出来的文件(新包没有的)要删掉 */
    if (old && upgrade) {
        char op[700];
        db_list_path(op, sizeof op, old->nvra);
        FILE *f = fopen(op, "r");
        int ndel = 0;
        if (f) {
            char line[1024];
            while (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!line[0])
                    continue;
                int innew = 0;
                for (long i = 0; i < x.n; i++)
                    if (!strcmp(x.paths[i], line)) {
                        innew = 1;
                        break;
                    }
                if (innew)
                    continue;
                char full[1120];
                snprintf(full, sizeof full, "%s%s", opt_root, line);
                struct stat st;
                if (lstat(full, &st) != 0)
                    continue;
                if (S_ISDIR(st.st_mode)) {
                    if (rmdir(full) == 0)
                        ndel++;
                } else if (unlink(full) == 0)
                    ndel++;
            }
            fclose(f);
        }
        printf("rpm: 升级顺带删掉旧包多出的 %d 个文件\n", ndel);
        char mp[700];
        db_meta_path(mp, sizeof mp, old->nvra);
        unlink(mp);
        unlink(op);
        static const char *const sc[] = { "prein", "postin", "preun",
                                          "postun", NULL };
        for (int i = 0; sc[i]; i++) {
            char sop[700];
            scr_path(sop, sizeof sop, old->nvra, sc[i]);
            unlink(sop);
        }
    }
    for (long i = 0; i < x.n; i++)
        free(x.paths[i]);
    free(x.paths);

    if (run_script_path(spath[1], sprog[1], arg) < 0)
        fprintf(stderr, "rpm: %s 的 %%post 失败(文件已落盘, 状态已记)\n", m.name);

    printf("rpm: 已安装 %s-%s-%s.%s\n", m.name, m.version, m.release, m.arch);
    close(lockfd);
    free(ents);
    if (owned)
        free(pl);
    free(img);
    return 0;
}

/* ------------------------- 卸载 ------------------------- */
/* 这个路径是否被**别的**已装包声明过。多版本并存时不能把别人还要的文件删了
 * (上游 rpm 卸载时同样做这项归属检查)。 */
static int owned_elsewhere(struct dbent *ents, int n, const char *self_nvra,
                           const char *path)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(ents[i].nvra, self_nvra))
            continue;
        char p[700];
        db_list_path(p, sizeof p, ents[i].nvra);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strcmp(line, path)) {
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

static int do_erase(const char *spec)
{
    struct dbent *ents = calloc(512, sizeof *ents);
    int nent = ents ? db_load(ents, 512) : 0;
    const struct dbent *k = ents ? db_find(ents, nent, spec) : NULL;
    if (!k) {
        fprintf(stderr, "rpm: 包 %s 没装过(数据库里没有)\n", spec);
        free(ents);
        return 1;
    }
    int lockfd = take_lock();
    if (lockfd < 0) {
        free(ents);
        return 1;
    }
    char lp[700];
    db_list_path(lp, sizeof lp, k->nvra);
    printf("rpm: 准备卸载 %s\n", k->nvra);

    /* %preun 的参数: 0 = 真的在删; >0 = 装完还有别的版本在用(升级) */
    int nver = 0;
    for (int i = 0; i < nent; i++)
        if (!strcmp(ents[i].m.name, k->m.name))
            nver++;
    char arg[8];
    snprintf(arg, sizeof arg, "%d", nver > 1 ? 1 : 0);

    /* 卸载时包文件不在手上, 脚本从安装时落库的 <db>/scriptlets/ 里取 */
    char sop[700];
    scr_path(sop, sizeof sop, k->nvra, "preun");
    if (run_script_path(sop, k->m.preunprog[0] ? k->m.preunprog : NULL, arg) < 0) {
        fprintf(stderr, "rpm: %s 的 %%preun 失败, 拒绝卸载(一个文件都没删)\n",
                k->m.name);
        close(lockfd);
        free(ents);
        return 1;
    }
    int nfile = 0, ndir = 0, nkeep = 0, nshared = 0;
    FILE *f = fopen(lp, "r");
    if (f) {
        char **v = NULL;
        int nv = 0, cap = 0;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0])
                continue;
            if (nv == cap) {
                cap = cap ? cap * 2 : 256;
                char **t = realloc(v, (size_t)cap * sizeof *v);
                if (!t)
                    break;
                v = t;
            }
            v[nv++] = strdup(line);
        }
        fclose(f);
        for (int i = nv - 1; i >= 0; i--) {
            if (owned_elsewhere(ents, nent, k->nvra, v[i])) {
                nshared++;                      /* 别的已装包还要它, 留着 */
                free(v[i]);
                continue;
            }
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
                    ndir++;
            } else {
                ok = (unlink(full) == 0);
                if (ok)
                    nfile++;
            }
            if (!ok)
                nkeep++;
            free(v[i]);
        }
        free(v);
    }
    printf("rpm: 删 %s: %d 个文件", k->nvra, nfile);
    if (nshared)
        printf(", %d 个文件留给别的已装包", nshared);
    if (ndir)
        printf(", %d 个空目录", ndir);
    if (nkeep)
        printf(", %d 个条目没删掉(非空目录或权限)", nkeep);
    printf("\n");
    char mp[700];
    db_meta_path(mp, sizeof mp, k->nvra);
    unlink(mp);
    unlink(lp);
    /* %postun 用刚删掉的那段脚本(还在水里, 上面只删了 meta/list) */
    scr_path(sop, sizeof sop, k->nvra, "postun");
    if (run_script_path(sop, k->m.postunprog[0] ? k->m.postunprog : NULL, arg) < 0)
        fprintf(stderr, "rpm: %s 的 %%postun 失败(文件已删, 状态已清)\n",
                k->m.name);
    static const char *const scx[] = { "prein", "postin", "preun", "postun",
                                       NULL };
    for (int i = 0; scx[i]; i++) {
        scr_path(sop, sizeof sop, k->nvra, scx[i]);
        unlink(sop);
    }
    sync();
    printf("rpm: 已卸载 %s\n", k->m.name);
    close(lockfd);
    free(ents);
    return 0;
}

/* ------------------------- 查询 ------------------------- */
static void print_meta(const struct meta *m, int verbose)
{
    printf("Name        : %s\n", m->name);
    printf("Version     : %s\n", m->version);
    printf("Release     : %s\n", m->release);
    printf("Architecture: %s\n", m->arch);
    printf("Install Date: (见 %s/installed/*.meta)\n", opt_db);
    printf("Size        : %ld\n", m->installsize);
    if (m->license[0])
        printf("License     : %s\n", m->license);
    if (m->summary[0])
        printf("Summary     : %s\n", m->summary);
    if (verbose && m->descr[0])
        printf("Description :\n%s\n", m->descr);
}

static int list_files_of(const char *nvra_s)
{
    char p[700];
    db_list_path(p, sizeof p, nvra_s);
    FILE *f = fopen(p, "r");
    if (!f) {
        fprintf(stderr, "rpm: %s 没有文件清单\n", nvra_s);
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof line, f))
        fputs(line, stdout);
    fclose(f);
    return 0;
}

static int qf(const char *path)
{
    struct dbent *ents = calloc(512, sizeof *ents);
    int n = ents ? db_load(ents, 512) : 0;
    char rel[1100];
    if (pc_member_path("", path, rel, sizeof rel) != 0)
        snprintf(rel, sizeof rel, "%s", path);
    int hit = 0;
    for (int i = 0; i < n; i++) {
        char p[700];
        db_list_path(p, sizeof p, ents[i].nvra);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strcmp(line, rel)) {
                printf("%s owns %s\n", ents[i].nvra, path);
                hit = 1;
            }
        }
        fclose(f);
    }
    free(ents);
    if (!hit)
        fprintf(stderr, "file %s belongs to no package\n", path);
    return hit ? 0 : 1;
}

/* 把 -q 系列的"选段"旗标收集起来: -q -i -l -p -f -a */
struct qsel {
    int info, files, pkgtree, all, ownsfile;
    const char *arg;
};

/* -qpl 用: 只列负载里的成员名, 不落盘。
 * 打的是**绝对路径**(与上游 rpm -qpl 一致): cpio 成员名已去掉 ./ 前缀。 */
static int pl_list_cb(const struct pc_mem *m, void *ud)
{
    long *n = (long *)ud;
    printf("/%s\n", m->name);
    (*n)++;
    return 0;
}

static int do_query(const struct qsel *qs)
{
    struct hdr h;
    unsigned char *img = NULL;
    long len = 0;
    if (qs->pkgtree) {
        if (!qs->arg) {
            fprintf(stderr, "rpm: -qp 需要包文件路径\n");
            return 1;
        }
        img = pc_slurp(qs->arg, &len);
        if (!img) {
            fprintf(stderr, "rpm: 读 %s 失败\n", qs->arg);
            return 1;
        }
        if (hdr_open(&h, img, len) < 0) {
            free(img);
            return 1;
        }
        struct meta m;
        meta_from_hdr(&m, &h);
        if (qs->files) {
            unsigned char *pl = NULL;
            long pllen = 0, n = 0;
            int owned = 0;
            if (payload_get(&h, &pl, &pllen, &owned) < 0) {
                free(img);
                return 1;
            }
            pc_cpio_walk(pl, pllen, pl_list_cb, &n);
            if (!n) {
                fprintf(stderr, "rpm: %s 的负载里一个成员都没有\n", qs->arg);
                if (owned)
                    free(pl);
                free(img);
                return 1;
            }
            if (owned)
                free(pl);
            free(img);
            return 0;
        }
        if (qs->info)
            print_meta(&m, 1);
        else
            printf("%s-%s-%s.%s\n", m.name, m.version, m.release, m.arch);
        free(img);
        return 0;
    }
    struct dbent *ents = calloc(512, sizeof *ents);
    int n = ents ? db_load(ents, 512) : 0;
    int rc = 0;
    if (qs->ownsfile) {
        rc = qf(qs->arg ? qs->arg : "");
        free(ents);
        return rc;
    }
    if (qs->all || !qs->arg) {
        for (int i = 0; i < n; i++) {
            if (qs->info)
                print_meta(&ents[i].m, 1);
            else if (qs->files)
                list_files_of(ents[i].nvra);
            else
                printf("%s\n", ents[i].nvra);
        }
        if (!n)
            printf("no packages installed\n");
        free(ents);
        return 0;
    }
    const struct dbent *k = db_find(ents, n, qs->arg);
    if (!k) {
        fprintf(stderr, "package %s is not installed\n", qs->arg);
        free(ents);
        return 1;
    }
    if (qs->info)
        print_meta(&k->m, 1);
    else if (qs->files)
        rc = list_files_of(k->nvra);
    else
        printf("%s\n", k->nvra);
    free(ents);
    return rc;
}

static void usage(void)
{
    printf("rpm (Parlz 移植实现 " RPM_PORT_VERSION ") —— .rpm 包的底层安装器\n");
    printf("用法: rpm [选项] 动作\n");
    printf("  -i|--install <包.rpm>...     安装\n");
    printf("  -U|--upgrade <包.rpm>...     升级(装新的, 删旧包多出的文件)\n");
    printf("  -e|--erase <包>...           卸载\n");
    printf("  -q <包>  -qa  -qi <包>  -ql <包>  -qf <路径>\n");
    printf("  -qp <包.rpm>  -qpi <包.rpm>  -qpl <包.rpm>\n");
    printf("  --compare-versions <a> <op> <b>   RPM vercmp(真=0)\n");
    printf("选项: --root <目录> --dbpath <目录> --noscripts --force\n");
    printf("      --oldpackage --replacepkgs --nodeps(本实现默认不解析依赖)\n");
    printf("      -v -h --version (只接受这些上游习惯的旗标)\n");
}

/* -qa / -qpi / -qp 这类粘连写法拆成可判断的选段 */
static int parse_q(const char *a, struct qsel *qs)
{
    if (a[0] != '-' || !strchr(a, 'q'))
        return 0;
    for (const char *p = a + 1; *p; p++) {
        if (*p == 'q')
            continue;
        if (*p == 'a')
            qs->all = 1;
        else if (*p == 'i')
            qs->info = 1;
        else if (*p == 'l')
            qs->files = 1;
        else if (*p == 'p')
            qs->pkgtree = 1;
        else if (*p == 'f')
            qs->ownsfile = 1;
        else
            return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *act = NULL;
    const char *arg = NULL;
    struct qsel qs;
    memset(&qs, 0, sizeof qs);
    char *args[64];
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(a, "--version") || !strcmp(a, "-v")) {
            printf("RPM 包管理器(Parlz 移植实现) " RPM_PORT_VERSION "\n");
            printf("  本机架构 %s; 支持 cpio+gzip 负载; 不验签名、不解析依赖\n",
                   host_arch());
            return 0;
        }
        if (!strcmp(a, "--root")) {
            if (++i >= argc) {
                fprintf(stderr, "rpm: --root 需要参数\n");
                return 2;
            }
            snprintf(opt_root, sizeof opt_root, "%s", argv[i]);
            char *sl = opt_root + strlen(opt_root) - 1;
            while (sl > opt_root && *sl == '/')
                *sl-- = 0;
            continue;
        }
        if (!strcmp(a, "--dbpath")) {
            if (++i >= argc) {
                fprintf(stderr, "rpm: --dbpath 需要参数\n");
                return 2;
            }
            snprintf(opt_db, sizeof opt_db, "%s", argv[i]);
            continue;
        }
        if (!strcmp(a, "--noscripts")) { opt_noscripts = 1; continue; }
        if (!strcmp(a, "--force")) { opt_force = 1; continue; }
        if (!strcmp(a, "--oldpackage")) { opt_oldpkg = 1; continue; }
        if (!strcmp(a, "--replacepkgs")) { opt_replace = 1; continue; }
        if (!strcmp(a, "--nodeps") || !strcmp(a, "--replacefiles") ||
            !strcmp(a, "--ignorearch") || !strcmp(a, "--ignoreos") ||
            !strcmp(a, "--nopostun") || !strcmp(a, "--nopostin") ||
            !strcmp(a, "--noverify") || !strcmp(a, "--excludedocs") ||
            !strcmp(a, "--includedocs") || !strcmp(a, "--quiet") ||
            !strcmp(a, "-vv") || !strcmp(a, "--nosignature"))
            continue;                          /* 上游旗标, 本实现里是空操作 */
        if (!strcmp(a, "--compare-versions")) { act = "compare"; continue; }
        if (!strcmp(a, "-i") || !strcmp(a, "--install")) { act = "install"; continue; }
        if (!strcmp(a, "-U") || !strcmp(a, "--upgrade")) { act = "upgrade"; continue; }
        if (!strcmp(a, "--reinstall")) { act = "upgrade"; opt_replace = 1; continue; }
        if (!strcmp(a, "-e") || !strcmp(a, "--erase") ||
            !strcmp(a, "--uninstall")) { act = "erase"; continue; }
        if (!strcmp(a, "--initdb")) {
            paths_init();
            char d[600];
            db_dir(d, sizeof d);
            printf("rpm: 数据库目录就绪 %s\n", d);
            return 0;
        }
        if (parse_q(a, &qs)) {
            act = "query";
            continue;
        }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "rpm: 不认的选项 %s(见 rpm --help)\n", a);
            return 2;
        }
        if (nargs < 64)
            args[nargs++] = a;
    }
    paths_init();

    if (!strcmp(act ?: "", "compare")) {
        if (nargs != 3) {
            fprintf(stderr, "rpm: --compare-versions 需要 3 个参数\n");
            return 2;
        }
        return pc_rpm_ver_match(args[0], args[1], args[2]) ? 0 : 1;
    }
    if (!strcmp(act ?: "", "install") || !strcmp(act ?: "", "upgrade")) {
        int up = !strcmp(act, "upgrade");
        if (!nargs) {
            fprintf(stderr, "rpm: %s 需要 .rpm 路径\n", act);
            return 2;
        }
        int rc = 0;
        for (int i = 0; i < nargs; i++)
            if (do_install(args[i], up) && !rc)
                rc = 1;
        return rc;
    }
    if (!strcmp(act ?: "", "erase")) {
        if (!nargs) {
            fprintf(stderr, "rpm: erase 需要包名\n");
            return 2;
        }
        int rc = 0;
        for (int i = 0; i < nargs; i++)
            if (do_erase(args[i]) && !rc)
                rc = 1;
        return rc;
    }
    if (!strcmp(act ?: "", "query")) {
        qs.arg = nargs ? args[0] : NULL;
        return do_query(&qs);
    }
    usage();
    return 2;
}
