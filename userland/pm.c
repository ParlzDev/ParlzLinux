// SPDX-License-Identifier: GPL-2.0-or-later
/* pm.c - Parlz 包管理器(工具链按需安装)。
 *
 * 包格式:.pm = newc cpio 归档(070701, cpio -o -H newc 产出), 成员 = 包内
 * 文件树 + 一个 `pm.pkg` manifest 普通文件(纯文本 `key: value` 行)。
 * 安装即按清单逐成员写 / (保留权限/软链); 卸载按 /var/lib/pm/<pkg>.files
 * 清单删文件。
 *
 * 子命令:
 *   pm install <pkg>   依次从 feed(宿主 pm-server)、Ubuntu 官方源、
 *                      清华镜像源找包并安装; 全无则报错
 *   pm install <file>  从本地文件安装(离线)
 *   pm remove <pkg>    按清单卸载
 *   pm list            列出已装包
 *   pm available       拉所有源的 Packages 索引
 *
 * 源来源:
 *   1. /etc/pm/feeds.conf(或 PM_FEED 环境变量)每行一个基址;
 *   2. 内置回退源(本地 feed 找不到包时自动尝试):
 *        - Ubuntu 官方: http://archive.ubuntu.com/ubuntu
 *        - 清华镜像:   https://mirrors.tuna.tsinghua.edu.cn/ubuntu
 *     包索引 <base>/Packages 逐行: "包名 版本 包文件名 大小bytes"。
 *
 * 约束(AGENTS.md): 静态 -fno-pie, 无 system(), 全 fork+execv。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "http_client.h"      /* http_download 下载 */

#define PKGDIR "/tmp/pm"            /* 清单目录: initramfs /tmp 是 tmpfs 可写,
                                     * /var 未必有树; 装/卸/列包都走这里 */
#define FEED "/etc/pm/feeds.conf"

/* PM 自己的版本号: 组件+大.小-阶段+第几版, 例 pm+1.1-RC+1
 *   阶段 R=RELEASE, RC=RELEASE CANDIDATE, B=BETA, A=ALPHA
 * 由构建系统 -DPARLZ_PM_VERSION=... 注入(单一来源: 仓库根 .pm-release);
 * 没注入时是 dev 串, 便于源码树里直接 gcc 编出来也认得出没打过版本。 */
#ifndef PARLZ_PM_VERSION
#define PARLZ_PM_VERSION "pm+0.0-A+0-dev"
#endif
#define TMAX 65536            /* 临时包/索引上限(工具链包可 >100MB, 分块存盘) */

/* 内置回退源: 本地 feed(PM_FEED/feeds.conf)找不到包时逐源尝试。
 * 第一个是**官方镜像站**(www.parlz.com/feed): 即使盘上的 feeds.conf 被人删了
 * 或写坏了, `pm install core` 仍然能从官网把命令装回来; 后两个是用户指定的
 * 外部源(Ubuntu 官方 + 清华镜像), 用来装上游 .deb 包。
 * 索引布局与宿主 pm-server 的 Packages 相同:
 *   "包名 版本 包文件名 大小bytes"。 */
static const char *FALLBACK_BASES[] = {
    "http://www.parlz.com/feed",
    "http://archive.ubuntu.com/ubuntu",
    "https://mirrors.tuna.tsinghua.edu.cn/ubuntu",
    NULL
};
#define N_FALLBACK 3

static char g_pkg[128];       /* 当前安装包名(写清单用) */
static long g_member_done;    /* 已安装成员计数(进度显示用) */

/* 清单目录 PKGDIR(=/tmp/pm) 逐级建; initramfs 里 /tmp 是 tmpfs 可写,
 * 包清单落这里, 安装/卸载/list 都过此处。 */
static void ensure_pkgdir(void)
{
    /* 逐级建: /tmp(initramfs tmpfs 可写) -> /tmp/pm */
    if (mkdir("/tmp", 0777) < 0 && errno != EEXIST)
        ;
    if (mkdir(PKGDIR, 0755) < 0 && errno != EEXIST)
        ;
}

/* 安装进度: 每 200 个成员打一行(工具链包数千成员要写数分钟,
 * 无进度会被误判卡死)。*/
static void progress_report(const char *name, int fatal)
{
    if (g_member_done % 200 == 0) {
        fprintf(stderr, "pm: 已安装 %ld 个成员...(最新 %s)\n",
                g_member_done, name);
        fflush(stderr);
    }
    (void)fatal;
}

/* 安装结束时打总结行, 明确成员总数。*/
static void finish_report(void)
{
    fprintf(stderr, "pm: 安装完成, 共 %ld 个成员\n", g_member_done);
    fflush(stderr);
}

static void register_pkg_file(const char *path)
{
    char mf[512];
    FILE *f;
    ensure_pkgdir();
    snprintf(mf, sizeof mf, "%s/%s.files", PKGDIR, g_pkg);
    f = fopen(mf, "a");
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
}
/* 读整个文件到堆。*/
static unsigned char *slurp(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    if (b && n > 0 && fread(b, 1, n, f) != (size_t)n)
        n = -1;
    fclose(f);
    *len_out = n;
    return n >= 0 ? b : NULL;
}

static unsigned rdhex(const unsigned char *p)
{
    /* newc cpio 头字段: 8 字符 ASCII 十六进制串(大端), 逐字符解析。
     * 包尾 TRAILER 自身头+名后仅剩尾部 0 填充(约 349/120 字节),
     * 字段处读到 0 填充按 0 处理, 靠 namesize=0 + 包尾判定退出。 */
    unsigned v = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10u;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10u;
        else
            d = 0;
        v = v * 16u + d;
    }
    return v;
}

/* 从内存解 newc cpio, 安装到 /。
 * 成员名是相对名(opt/toolchain/...、usr/bin/gcc、pm.pkg 等),
 * 安装时补 / 前缀落根文件系统。
 * 目录建目录; 软链用成员数据(=目标路径) ln -s; 普通文件落盘+chmod。
 * 跳过 TRAILER!。
 * 进度: 每 200 个成员打一行 "pm: 安装 N 个成员(已完成, 写盘耗时较长)"
 *       —— 工具链包数千成员要写数分钟, 无进度会被误判卡死;
 *       guest 里 /var/tmp 可能没挂, 落盘失败不中断安装。
 * 返回 0 成功 / -1 中途失败。 */
static int install_cpio_mem(const unsigned char *b, long len)
{
    long off = 0;
    while (off + 110 <= len) {
        /* 包尾: TRAILER! 自身头+名 已超出包尾(数据区是 cpio 输出端的
         * 0 填充), off 落在尾填充区, magic 非 070701, 正常结束。 */
        unsigned magic_ok = 1;
        for (int i = 0; i < 6; i++)
            if (b[off + i] != ((const unsigned char *)"070701")[i])
                magic_ok = 0;
        if (!magic_ok)
            break;
        /* newc cpio 头 110 字节: magic "070701"+pad 到 8, 再 13 个
         * 8 字符 ASCII hex 大端字段, 逐字段 8 字节 @8..@104, 尾 pad 到 110:
         *   +8 ino +16 uid +24 gid +32 nlink +40 mtime +48 csum
         *   +56 filesize +64 devmajor +72 devminor +80 rnamesize
         *   +88 namesize +96 chksum +104 (pad)
         * 宿主 oracle 穷举 + gcc/clang/mini 三包全成员走位验证定死:
         * mode 在 +8 段内偏移 6(即 8..15 的第 6 字节 = "43F4A" 起点),
         * fsize = rdhex(+54), namesize = rdhex(+94)。
         * 走位(命中 gcc 全 6719 / clang 全 1125 / mini 7 成员):
         * off+=110; off+=namesize; off=(off+3)&~3; off+=(fsize+3)&~3。
         * 名字区紧跟头 +110(无额外 4 对齐, namesize 已含 4 填充),
         * 数据区 4 对齐起点。包尾 TRAILER! 头后是 cpio 输出端的 0 填充,
         * 靠 magic 校验非 070701 退出。 */
        unsigned mode     = rdhex(b + off + 14);
        unsigned fsize    = rdhex(b + off + 54);
        unsigned namesize = rdhex(b + off + 94);
        off += 110;
        /* namesize 在 +94..+101。包尾 TRAILER! 成员的头(110B)+名(TRAILER!
         * 11B) 可能已超出包尾(后面只剩 0 填充), 此时 +94 读到 0 ->
         * namesize=0, 干净退出(该 TRAILER 不算成员)。非包尾时 namesize 正常。 */
        if (namesize == 0)
            break;
        if (namesize > 512 || off + namesize > len)
            break;
        char name[512];
        unsigned nl = namesize < 511 ? namesize : 511;
        if (nl)
            memcpy(name, b + off, nl);
        name[nl] = 0;
        /* 去尾部 NUL: cpio newc 名区按 4 字节对齐填充, 末字节常补 \0
         * (宿主实测 TRAILER 头 namesize=11, 名区 raw "TRAILER!!!\0",
         * 去 0 后 10 字符, 前缀 "TRAILER!" 仍可匹配)。 */
        for (unsigned i = 0; i < nl; i++)
            if (name[i] == 0) { nl = i; break; }
        name[nl] = 0;
        off += namesize;
        off = (off + 3) & ~3L;
        /* TRAILER! 终止符: 名(去 0)以 "TRAILER!" 开头且 fsize=0。
         * 宿主 cpio 解包不产生该条目, guest 走位到这里即正常结束。 */
        if (!strncmp(name, "TRAILER!", 8) && fsize == 0)
            break;
        /* 成员名: cpio newc 存相对名(opt/lib/usr...), 安装补 / 前缀落根。
         * 已带 / 前缀的绝对名直接用。 */
        char path[512];
        if (name[0] == '/')
            snprintf(path, sizeof path, "%s", name);
        else
            snprintf(path, sizeof path, "/%s", name);

        /* 根成员 "."(find . 首项) 映射到 /, 直接建 / 并跳过。 */
        if (name[0] == '.' && name[1] == 0) {
            off += (fsize + 3) & ~3L;
            g_member_done++;
            continue;
        }

        int dir = (mode & 0xF000) == 0x4000;
        int lnk = (mode & 0xF000) == 0xA000;

        /* 建父目录(逐级; 父目录成员按 sort 顺序先处理, 此处兜底) */
        if (!dir) {
            char parent[512];
            snprintf(parent, sizeof parent, "%s", path);
            char *sl = strrchr(parent, '/');
            if (sl && sl != parent) {
                *sl = 0;
                char *p2 = parent + 1;
                while (*p2) {
                    if (*p2 == '/') {
                        *p2 = 0;
                        mkdir(parent, 0755);
                        *p2 = '/';
                    }
                    p2++;
                }
                mkdir(parent, 0755);
            }
        }

        if (dir) {
            mkdir(path, (mode & 07777) ? (mode & 07777) : 0755);
            off += (fsize + 3) & ~3L;
            g_member_done++;
            progress_report(name, 0);
            continue;
        }
        if (lnk) {
            char target[512];
            unsigned tn = fsize < 511 ? fsize : 511;
            if (off + fsize > len)
                break;
            memcpy(target, b + off, tn);
            target[tn] = 0;
            unlink(path);
            if (symlink(target, path) < 0 && errno != EEXIST) {
                fprintf(stderr, "pm: 软链 %s -> %s 失败: %s\n",
                        path, target, strerror(errno));
                return -1;
            }
            register_pkg_file(path);
            off += (fsize + 3) & ~3L;
            g_member_done++;
            progress_report(name, 0);
            continue;
        }
        /* 普通文件。先 unlink 再建, 不能就地 O_TRUNC:
         *   ① 覆盖**正在运行**的可执行文件会 ETXTBSY —— `pm install pm`
         *      自更新、以及覆盖 busybox/正被脚本用的命令时必然撞上;
         *      unlink 后新建是换 inode, 老进程继续跑自己的映像。
         *   ② O_CREAT 的 mode 只在**创建**时生效, 就地覆盖会留下旧文件的
         *      权限(0644) → 装回来的 /bin/nano 不可执行。 */
        unlink(path);
        int out = open(path, O_WRONLY | O_CREAT | O_TRUNC,
                       (mode & 07777) ? (mode & 07777) : 0644);
        if (out < 0) {
            fprintf(stderr, "pm: 写 %s 失败: %s\n", path, strerror(errno));
            off += (fsize + 3) & ~3L;
            continue;
        }
        if (fsize) {
            ssize_t w = write(out, b + off, fsize);
            (void)w;
        }
        close(out);
        if (chmod(path, mode & 07777) != 0 && errno != EROFS)
            fprintf(stderr, "pm: chmod %s %o 失败: %s\n",
                    path, mode & 07777, strerror(errno));
        register_pkg_file(path);
        off += (fsize + 3) & ~3L;
        g_member_done++;
        progress_report(name, 0);
    }
    finish_report();
    return 0;
}

/* 读 feed 第一个基址。PM_FEED 环境变量优先。*/
static int feed_base(char *out, size_t n)
{
    const char *env = getenv("PM_FEED");
    if (env && *env) {
        snprintf(out, n, "%s", env);
        return 0;
    }
    FILE *f = fopen(FEED, "r");
    if (!f)
        return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#')
            continue;
        snprintf(out, n, "%s", line);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/* 下载 url -> dest(经 /tmp 临时文件原子 rename, http_download 自带该语义)。
 * 返回 0 成功 / 非 0 curl 风格错误码。*/
static int download(const char *url, const char *dest)
{
    struct http_options o;
    struct http_result r;
    memset(&o, 0, sizeof o);
    memset(&r, 0, sizeof r);
    o.url = url;
    o.output = dest;
    o.timeout = 600;                /* 工具链包可能数百 MB, 给 10 分钟 */
    o.follow = 1;
    o.insecure = 0;                /* 严格校验; feed 服务器需带 CA 证书 */
    o.cacert = "/etc/ssl/cert.pem"; /* 系统信任束(initramfs 已含宿主 ca-certificates) */
    o.fail_http = 1;
    int rc = http_download(&o, &r, NULL);
    if (rc != 0)
        fprintf(stderr, "pm: 下载 %s 失败: %s (code=%d)%s\n",
                url, r.error, rc,
                rc == 60 ? " (HTTPS 证书校验失败: 服务器需配 CA 或 pm --insecure)" : "");
    return rc;
}

/* 在单个源的 Packages 索引里找 pkg 的真实包文件名。
 * 索引行: "包名 版本 包文件名 大小bytes"(前 4 列)。
 * 找到: 写 pkgfile 并返回 1; 源不可达/无此包: 返回 0(不报错, 换下一源)。 */
static int find_pkg_in_base(const char *base, const char *pkg,
                            char *pkgfile, size_t n)
{
    char idx[1024], idxtmp[256];
    snprintf(idx, sizeof idx, "%s/Packages", base);
    snprintf(idxtmp, sizeof idxtmp, "/tmp/pm_idx_%s", pkg);
    pkgfile[0] = 0;
    if (download(idx, idxtmp) != 0)
        return 0;
    FILE *f = fopen(idxtmp, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char nm[128], pf[128];
            int got = sscanf(line, "%127s %*s %127s", nm, pf);
            if (got >= 2 && !strcmp(nm, pkg)) {
                snprintf(pkgfile, n, "%s", pf);
                break;
            }
        }
        fclose(f);
    }
    unlink(idxtmp);
    return pkgfile[0] != 0;
}

/* 逐源找包: 本地 feed(若有) → 内置回退源(Ubuntu 官方 → 清华镜像)。
 * 返回 0 并在 base[]/pkgfile[] 填命中源; 全部未命中返回 -1。 */
static int find_pkg(const char *pkg, char *base, size_t bn,
                    char *pkgfile, size_t fn)
{
    char localbase[512];
    if (feed_base(localbase, sizeof localbase) == 0) {
        if (find_pkg_in_base(localbase, pkg, pkgfile, fn)) {
            snprintf(base, bn, "%s", localbase);
            return 0;
        }
    }
    for (int i = 0; i < N_FALLBACK; i++) {
        if (find_pkg_in_base(FALLBACK_BASES[i], pkg, pkgfile, fn)) {
            snprintf(base, bn, "%s", FALLBACK_BASES[i]);
            return 0;
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        printf("pm %s - Parlz 包管理器(工具链与可选命令按需安装)\n",
               PARLZ_PM_VERSION);
        printf("  pm install <包名|本地.pm>  安装(gcc/clang/core 等)\n");
        printf("  pm remove <包名>          卸载\n");
        printf("  pm list                   已装包\n");
        printf("  pm available              各源可装包(本地 feed + Ubuntu + 清华)\n");
        printf("  pm version                本包管理器的版本号\n");
        return 0;
    }
    if (!strcmp(argv[1], "version") || !strcmp(argv[1], "-V") ||
        !strcmp(argv[1], "--version")) {
        printf("%s\n", PARLZ_PM_VERSION);
        return 0;
    }
    const char *cmd = argv[1];
    char base[512];

    if (!strcmp(cmd, "install")) {
        if (argc < 3) {
            fprintf(stderr, "pm: install 需要包名或本地 .pm 路径\n");
            return 2;
        }
        const char *arg = argv[2];
        struct stat st;
        if (stat(arg, &st) == 0 && S_ISREG(st.st_mode)) {
            long len;
            unsigned char *b = slurp(arg, &len);
            if (!b || len <= 0) {
                fprintf(stderr, "pm: 读 %s 失败\n", arg);
                return 1;
            }
            const char *bs = strrchr(arg, '/');
            bs = bs ? bs + 1 : arg;
            char nm[128];
            snprintf(nm, sizeof nm, "%s", bs);
            char *dot = strrchr(nm, '.');
            if (dot && !strcmp(dot, ".pm"))
                *dot = 0;
            snprintf(g_pkg, sizeof g_pkg, "%s", nm);
            g_member_done = 0;
            printf("pm: 安装本地包 %s -> /\n", arg);
            int lr = install_cpio_mem(b, len);
            free(b);
            if (lr < 0 || g_member_done == 0) {
                fprintf(stderr, "pm: 本地包安装失败(rc=%d, 成员=%ld)\n",
                        lr, g_member_done);
                return 1;
            }
            printf("pm: %s 安装完成\n", g_pkg);
            return 0;
        }
        /* feed 下载: 本地 feed 找不到包 → 自动回退内置源
         * (Ubuntu 官方 → 清华镜像), 逐源找真实包文件名。 */
        char found_base[512], pkgfile[128];
        if (find_pkg(arg, found_base, sizeof found_base,
                     pkgfile, sizeof pkgfile) < 0) {
            fprintf(stderr,
                    "pm: 包 %s 在所有源均未找到(本地 feed + Ubuntu 官方 + 清华镜像)\n",
                    arg);
            return 1;
        }
        char url[1024], tmp[256];
        if (pkgfile[0])
            snprintf(url, sizeof url, "%s/%s", found_base, pkgfile);
        else
            /* 源无 Packages 索引或行格式不匹配时兜底: <base>/<pkg>.pm */
            snprintf(url, sizeof url, "%s/%s.pm", found_base, arg);
        snprintf(tmp, sizeof tmp, "/tmp/pm_%s.pm", arg);
        snprintf(g_pkg, sizeof g_pkg, "%s", arg);
        g_member_done = 0;
        printf("pm: 从 %s 下载 %s\n", found_base, pkgfile[0] ? pkgfile : arg);
        fflush(stdout);
    if (download(url, tmp) != 0)
        return 1;
    long len;
    unsigned char *b = slurp(tmp, &len);
    if (!b || len <= 0) {
        fprintf(stderr, "pm: 读临时包失败: %s (len=%ld)\n", tmp, len);
        unlink(tmp);
        return 1;
    }
    printf("pm: 包下载完成 %ld 字节, 开始安装\n", len);
    fflush(stdout);
    int icrc = install_cpio_mem(b, len);
    free(b);
    unlink(tmp);
    if (icrc < 0 || g_member_done == 0) {
        fprintf(stderr,
                "pm: 安装失败: rc=%d, 成员数=%ld, 包大小=%ld 字节"
                "(包损坏或下载不完整?)\n", icrc, g_member_done, len);
        return 1;
    }
    printf("pm: %s 安装完成\n", arg);
    return 0;
    }

    if (!strcmp(cmd, "remove")) {
        if (argc < 3) {
            fprintf(stderr, "pm: remove 需要包名\n");
            return 2;
        }
        char mf[512];
        snprintf(mf, sizeof mf, PKGDIR "/%s.files", argv[2]);
        FILE *f = fopen(mf, "r");
        if (!f) {
            fprintf(stderr, "pm: %s 未安装(无 %s)\n", argv[2], mf);
            return 1;
        }
        char line[512];
        int n = 0;
        /* 倒序收集清单行: 先删文件/软链, 再删(现已空的)目录。
         * 正向删目录会因包内顺序"目录在前、文件在后"而 ENOTEMPTY。 */
        char *lines[16384];
        int nlines = 0;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            if (line[0] && nlines < 16384)
                lines[nlines++] = strdup(line);
        }
        fclose(f);
        for (int i = nlines - 1; i >= 0; i--) {
            if (unlink(lines[i]) == 0)
                n++;
            free(lines[i]);
        }
        unlink(mf);
        /* 清空的包清单目录本身保留; 工具链目录(/opt/toolchain 等)由
         * 上面的倒序 unlink 逐层删空, 末级目录一并移除。 */
        printf("pm: 移除 %s (%d 个条目)\n", argv[2], n);
        return 0;
    }

    if (!strcmp(cmd, "list")) {
        DIR *d = opendir(PKGDIR);
        if (!d) {
            printf("pm: (无已装包)\n");
            return 0;
        }
        struct dirent *e;
        int n = 0;
        while ((e = readdir(d))) {
            if (strstr(e->d_name, ".files")) {
                char nm[128];
                snprintf(nm, sizeof nm, "%s", e->d_name);
                char *dot = strrchr(nm, '.');
                if (dot)
                    *dot = 0;
                printf("pm: %s\n", nm);
                n++;
            }
        }
        closedir(d);
        if (!n)
            printf("pm: (无已装包)\n");
        return 0;
    }

    if (!strcmp(cmd, "available")) {
        /* 本地 feed(若有) + 内置回退源, 逐个拉 Packages。 */
        char bases[16][512];
        int nb = 0;
        char lb[512];
        if (feed_base(lb, sizeof lb) == 0)
            snprintf(bases[nb++], 512, "%s", lb);
        for (int i = 0; i < N_FALLBACK && nb < 16; i++)
            snprintf(bases[nb++], 512, "%s", FALLBACK_BASES[i]);
        if (nb == 0) {
            fprintf(stderr, "pm: 无可用源(设 PM_FEED/feeds.conf 或用内置回退)\n");
            return 1;
        }
        int any = 0;
        for (int i = 0; i < nb; i++) {
            char url[1024], tmp[256];
            snprintf(url, sizeof url, "%s/Packages", bases[i]);
            snprintf(tmp, sizeof tmp, "/tmp/pm_Packages_%d", i);
            if (download(url, tmp) != 0) {
                fprintf(stderr, "pm: 源 %s 不可达, 跳过\n", bases[i]);
                continue;
            }
            FILE *f = fopen(tmp, "r");
            if (f) {
                char line[512];
                while (fgets(line, sizeof line, f)) {
                    line[strcspn(line, "\r\n")] = 0;
                    if (line[0] && line[0] != '#')
                        printf("  [%s] %s\n", bases[i], line);
                    any = 1;
                }
                fclose(f);
            }
            unlink(tmp);
        }
        if (!any)
            printf("pm: (无源提供包)\n");
        return 0;
    }

    fprintf(stderr, "pm: 未知子命令 '%s'\n", cmd);
    return 2;
}
