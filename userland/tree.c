/* tree.c - 树形展示目录。用法: tree [dir] [-a] [-L <depth>] [-c] [-d]
 * 无参数显示当前目录; -a 含隐藏项; -L 限深度(默认 3); -d 只看目录;
 * -c 强制彩色。
 * 颜色: 目录蓝、普通文件绿、符号链接青(管道输出或 NO_COLOR=1 时关,
 * LS_COLORS=1 强开,与 sh 的 ls 同一套开关)。
 * 末尾打印 "N directories, N files"。
 *
 * 循环防护(重要): 符号链接一律不递归(防环),目录递归靠 -L 深度限制;
 * 每层子项名先 readdir 全部拷进局部数组再打印(防悬垂 dirent* 指针),
 * 避免边打印边递归导致的栈深递归与重复列出。
 * 末尾的 summary 行在递归返回后统一打印一次,不在中间打印。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

static long n_dirs, n_files;
static int g_max_depth;
static volatile sig_atomic_t g_abort = 0;

static void sigint_cb(int sig)
{
    (void)sig;
    g_abort = 1;
}

static int color_on(void)
{
    if (getenv("NO_COLOR"))
        return 0;
    if (getenv("LS_COLORS"))
        return 1;
    return isatty(STDOUT_FILENO);
}

/* 按类型取 ANSI 颜色 */
static const char *type_color(int mode, int is_link)
{
    if (is_link)
        return "\033[36m";   /* 链接青 */
    if (S_ISDIR(mode))
        return "\033[34m";   /* 目录蓝 */
    if (S_ISREG(mode))
        return "\033[32m";   /* 普通文件绿 */
    return "";
}
#define ANSI_RESET "\033[0m"

/* 递归打印 dir 的**直接子项**。dir 本身已在父层(或 main)打印。
 * depth = 当前层号(根 0)。符号链接一律不递归(防环)。
 * 先 readdir 收集全部子项名到局部数组再打印,避免悬垂指针。
 * 末尾 summary 由 main 在递归返回后打印一次,这里不打印。
 */
static void show(const char *dir, int depth, int show_all, int dirs_only,
                 int col)
{
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        return;
    }
    /* 子项名拷进局部数组(不存 struct dirent* 指针,避免 closedir
     * 后悬垂),最多 256 项。 */
    char names[256][256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < 256) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (!show_all && e->d_name[0] == '.')
            continue;
        snprintf(names[n], sizeof names[n], "%s", e->d_name);
        n++;
    }
    closedir(d);
    /* 字典序冒泡 */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[256];
                snprintf(t, sizeof t, "%s", names[i]);
                snprintf(names[i], sizeof names[i], "%s", names[j]);
                snprintf(names[j], sizeof names[j], "%s", t);
            }

    for (int i = 0; i < n; i++) {
        if (g_abort)
            break;
        char full[1024];
        int last = (i == n - 1);
        snprintf(full, sizeof full, "%s/%s", dir, names[i]);

        struct stat st;
        int is_link = 0, is_dir = 0;
        if (lstat(full, &st) == 0)
            is_link = S_ISLNK(st.st_mode);
        if (is_link) {
            /* 链接按其指向算目录/文件(用于计数与颜色) */
            struct stat dst;
            if (stat(full, &dst) == 0)
                is_dir = S_ISDIR(dst.st_mode);
        } else {
            is_dir = S_ISDIR(st.st_mode);
        }
        if (is_dir)
            n_dirs++;
        else
            n_files++;

        /* dirs_only: 只列目录,文件跳过 */
        if (dirs_only && !is_dir)
            continue;

        /* 前缀:每层 4 字符;父层是否末项本层无法得知,退化为统一 "│   "
         * (末项子层用 "    " 视觉近似),树结构仍正确可读。 */
        char prefix[512];
        prefix[0] = 0;
        if (depth > 0) {
            int plen = 0;
            for (int p = 0; p < depth - 1; p++) {
                memcpy(prefix + plen, "│   ", 4);
                plen += 4;
            }
            /* 本层前缀 = 父项是否末项(未知)→ 用 "    " */
            memcpy(prefix + plen, "    ", 4);
            plen += 4;
            prefix[plen] = 0;
        }
        char line[1152];
        int w = 0;
        for (int k = 0; prefix[k]; k++)
            line[w++] = prefix[k];
        const char *branch = last ? "└── " : "├── ";
        while (*branch)
            line[w++] = *branch++;

        /* 名字(链接加 " -> target") */
        char disp[320];
        if (is_link) {
            char tgt[256];
            ssize_t tl = readlink(full, tgt, sizeof tgt - 1);
            if (tl > 0) {
                tgt[tl] = 0;
                snprintf(disp, sizeof disp, "%s -> %s", names[i], tgt);
            } else
                snprintf(disp, sizeof disp, "%s", names[i]);
        } else
            snprintf(disp, sizeof disp, "%s", names[i]);
        if (is_dir)
            strncat(disp, "/", sizeof disp - strlen(disp) - 1);

        const char *c = col ? type_color(st.st_mode, is_link) : "";
        if (col && c[0])
            w += snprintf(line + w, sizeof line - w, "%s%s%s",
                          c, disp, ANSI_RESET);
        else
            w += snprintf(line + w, sizeof line - w, "%s", disp);
        line[w] = 0;
        puts(line);

        /* 真目录才递归(链接/文件不递归,防环);深度受限。
         * 默认深度 3(根 0,子 1,孙 2,曾孙 3 不再递归);-L 可调。 */
        int ndepth = (g_max_depth < 0) ? 0 : g_max_depth;
        if (is_dir && !is_link && depth + 1 < ndepth && !g_abort)
            show(full, depth + 1, show_all, dirs_only, col);
    }
    /* 递归返回后,被 Ctrl+C 打断时打印中断行并提前退出,
     * 不再打印 "N directories, N files" 的汇总(数据不完整)。 */
    if (g_abort) {
        puts("");
        puts("tree: interrupted (Ctrl+C)");
        _exit(130);
    }
}

int main(int argc, char *argv[])
{
    int show_all = 0, force_color = 0, dirs_only = 0;
    int max_depth = 3;            /* 默认 3 层,防止无限递归 */
    const char *root = ".";
    int i = 1;
    while (i < argc) {
        const char *a = argv[i];
        if (!strcmp(a, "-a"))
            show_all = 1;
        else if (!strcmp(a, "-c") || !strcmp(a, "--color"))
            force_color = 1;
        else if (!strcmp(a, "-d"))
            dirs_only = 1;
        else if (!strncmp(a, "-L", 2)) {
            int v = (i + 1 < argc && argv[i + 1][0] != '-') ?
                    atoi(argv[++i]) : -1;
            max_depth = (v < 0) ? -1 : v;
        } else if (a[0] != '-')
            root = a;
        i++;
    }
    g_max_depth = max_depth;

    /* Ctrl+C: 装 SIGINT handler,遍历被打断时 g_abort=1 退出 */
    signal(SIGINT, sigint_cb);

    int col = force_color ? 1 : color_on();

    /* 根目录自身(带颜色) */
    struct stat rst;
    int rcol = 0;
    const char *rc = "";
    if (col) {
        if (lstat(root, &rst) == 0) {
            rcol = 1;
            rc = S_ISDIR(rst.st_mode) ? "\033[34m"
                 : S_ISREG(rst.st_mode) ? "\033[32m" : "";
        }
    }
    if (rcol && rc[0])
        printf("%s%s%s\n", rc, root, ANSI_RESET);
    else
        printf("%s\n", root);

    /* 根目录自身若是目录则计一次 */
    struct stat rst2;
    if (lstat(root, &rst2) == 0 && S_ISDIR(rst2.st_mode))
        n_dirs++;

    show(root, 0, show_all, dirs_only, col);

    /* 只打印一次 summary(在递归返回后,不在中间打印) */
    printf("\n%ld directories, %ld files\n", n_dirs, n_files);
    return 0;
}
