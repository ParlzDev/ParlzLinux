/* chmod.c - 修改文件权限(静态用户空间)。
 *
 * 用法: chmod [-R] [-v] MODE FILE...
 *   MODE 支持八进制(如 755、0644)与符号模式(如 u+x、go-w、a=rw、+X),
 *   符号子句用逗号分隔。多个文件逐个处理;-R 递归目录;-v 打印变化。
 * 退出码: 任一文件失败返回 1,全部成功返回 0。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int opt_recursive = 0;
static int opt_verbose = 0;

/* 解析符号模式并按当前模式计算新模式。
 * 语法: 子句[,子句...],每个子句 [ugoa...][+-=][rwxXst]。
 * X = 目录加执行位;文件仅在已有任一执行位时加。
 * 成功返回 0,语法错误返回 -1。 */
static int apply_symbolic(const char *spec, const struct stat *st, mode_t *out)
{
    mode_t m = st->st_mode & 07777;
    int isdir = S_ISDIR(st->st_mode);
    const char *p = spec;
    if (!*p)
        return -1;
    while (*p) {
        /* 谁: u/g/o/a;缺省 = a */
        int u = 0, g = 0, o = 0, saw_who = 0;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            saw_who = 1;
            if (*p == 'u') u = 1;
            else if (*p == 'g') g = 1;
            else if (*p == 'o') o = 1;
            else u = g = o = 1;
            p++;
        }
        if (!saw_who)
            u = g = o = 1;
        for (;;) {
            char op = *p;
            if (op != '+' && op != '-' && op != '=')
                return -1;
            p++;
            /* 收集 perms 字母 */
            int r = 0, w = 0, x = 0, bigx = 0, s = 0, t = 0, any = 0;
            while (*p && strchr("rwxXst", *p)) {
                any = 1;
                if (*p == 'r') r = 1;
                else if (*p == 'w') w = 1;
                else if (*p == 'x') x = 1;
                else if (*p == 'X') bigx = 1;
                else if (*p == 's') s = 1;
                else t = 1;
                p++;
            }
            if (!any)
                return -1;
            /* X 条件: 目录恒加执行位;文件需已有执行位 */
            int xeff = x || (bigx && (isdir || (m & 0111)));
            mode_t add = 0, clr = 0;
            if (u) {
                if (r) add |= 0400;
                if (w) add |= 0200;
                if (xeff) add |= 0100;
                if (s) add |= 04000;
                clr |= 0700 | (s ? 0 : 04000) | 04000;
            }
            if (g) {
                if (r) add |= 0040;
                if (w) add |= 0020;
                if (xeff) add |= 0010;
                if (s) add |= 02000;
                clr |= 0070 | 02000;
            }
            if (o) {
                if (r) add |= 0004;
                if (w) add |= 0002;
                if (xeff) add |= 0001;
                if (t) add |= 01000;
                clr |= 0007 | 01000;
            }
            if (op == '+')
                m |= add;
            else if (op == '-')
                m &= ~add;
            else {
                m &= ~clr;
                m |= add;
            }
            if (*p == ',') {
                p++;
                break;
            }
            if (!*p)
                break;
            /* 无分隔符: 同 who 继续下一个 op 子句(如 u+rwx) */
        }
    }
    *out = m;
    return 0;
}

/* 解析 MODE(八进制或符号)。成功 0,语法错误 -1。 */
static int parse_mode(const char *spec, const struct stat *st, mode_t *out)
{
    /* 全八进制数字 → 直接值 */
    size_t n = strlen(spec);
    if (n && strspn(spec, "01234567") == n) {
        long v = strtol(spec, NULL, 8);
        if (v >= 0 && v <= 07777) {
            *out = (mode_t)v;
            return 0;
        }
        return -1;
    }
    return apply_symbolic(spec, st, out);
}

/* 对单个路径应用 MODE。返回 0 成功,1 失败。 */
static int chmod_path(const char *spec, const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        return 1;
    }
    mode_t cur = st.st_mode & 07777;
    mode_t want;
    if (parse_mode(spec, &st, &want) < 0) {
        fprintf(stderr, "chmod: 无效模式 '%s'\n", spec);
        return 1;
    }
    mode_t apply;
    if (strspn(spec, "01234567") == strlen(spec) && spec[0])
        apply = want;                    /* 八进制: 直接覆盖全部权限位 */
    else
        apply = (st.st_mode & ~07777u) | (want & 07777);
    if (chmod(path, apply) < 0) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (opt_verbose)
        printf("mode of '%s': %04o -> %04o\n", path,
               (unsigned)cur, (unsigned)(apply & 07777));
    return 0;
}

static int chmod_walk(const char *spec, const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int rc = chmod_path(spec, path);
    if (opt_recursive && S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
            return 1;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char full[PATH_MAX];
            snprintf(full, sizeof full, "%s/%s", path, e->d_name);
            rc |= chmod_walk(spec, full);
        }
        closedir(d);
    }
    return rc;
}

int main(int argc, char *argv[])
{
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-R"))
            opt_recursive = 1;
        else if (!strcmp(argv[i], "-v"))
            opt_verbose = 1;
        else if (!strcmp(argv[i], "--"))
            break;
        else if (argv[i][0] == '-' && argv[i][1])
            break;                     /* "-x" 等以 - 开头的是符号模式,不是选项 */
        else
            break;
    }
    if (argc - i < 2) {
        fprintf(stderr, "用法: chmod [-R] [-v] MODE FILE...\n"
                        "  MODE: 八进制(755)或符号(u+x,go-w,a=rw,+X)\n");
        return 1;
    }
    const char *spec = argv[i++];
    int rc = 0;
    for (; i < argc; i++)
        rc |= chmod_walk(spec, argv[i]);
    return rc;
}
