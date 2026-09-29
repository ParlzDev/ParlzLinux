/* sed.c - 极简流编辑器。用法: sed <expr> [file...]
 * 支持的 expr 形式(本最小实现):
 *   s/old/new/      替换(old 为字面子串,非正则;末尾可加 /g 全局)
 *   /pat/           只打印含 pat 的行(字面匹配)
 *   p               打印每行(等价 cat)
 * 面向最小 shell:整行处理,字面子串替换,无地址范围。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int do_s(char *line, const char *expr, size_t len)
{
    /* expr 形如 "s/old/new/" 或 "s/old/new/g" */
    if (expr[0] != 's' || expr[1] != '/')
        return 0;
    const char *rest = expr + 2;
    int global = 0;
    /* 收集分隔符(默认 '/') */
    char sep = '/';
    /* 找第一个分隔符位置 */
    char *copy = strdup(rest);
    if (!copy)
        return 0;
    /* 解析 old/new[/g] */
    char *o = strchr(copy, sep);
    if (!o) { free(copy); return 0; }
    *o = 0;
    char *old = copy;
    char *new = o + 1;
    /* new 到下一个 sep 或末尾 */
    char *n2 = strchr(new, sep);
    char *repl = new;
    size_t repl_len;
    if (n2) {
        repl_len = (size_t)(n2 - new);
        /* 检查 /g */
        if (n2[1] == 'g')
            global = 1;
    } else {
        repl_len = strlen(new);
    }
    free(copy);

    size_t old_len = strlen(old);
    if (old_len == 0)
        return 0;
    char *w = line;
    const char *r = line;
    while (*r) {
        if (strncmp(r, old, old_len) == 0) {
            memcpy(w, repl, repl_len);
            w += repl_len;
            r += old_len;
            if (!global)
                break;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
    return 1;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: sed <s/old/new[/g] | /pat/ | p> [file...]\n");
        return 1;
    }
    const char *expr = argv[1];
    FILE *in = stdin;
    int nfiles = 0;
    for (int i = 2; i < argc; i++)
        nfiles++;
    for (int fi = 0; fi <= nfiles; fi++) {
        if (fi > 0) {
            in = fopen(argv[fi], "r");
            if (!in) {
                perror(argv[fi]);
                continue;
            }
        }
        char line[8192];
        while (fgets(line, sizeof line, in)) {
            size_t len = strcspn(line, "\n");
            if (expr[0] == 's') {
                char *l2 = strdup(line);
                if (l2) {
                    do_s(l2, expr, len);
                    if (line[len] == '\n')
                        l2[strlen(l2)] = '\n';
                    fwrite(l2, 1, strlen(l2), stdout);
                    free(l2);
                }
            } else if (expr[0] == '/') {
                /* /pat/ 只打印含 pat 的行 */
                char pat[256];
                const char *p = expr + 1;
                size_t pl = 0;
                while (p[pl] && p[pl] != '/' && pl < sizeof pat - 1)
                    pat[pl++] = p[pl];
                pat[pl] = 0;
                if (strstr(line, pat))
                    fputs(line, stdout);
            } else if (expr[0] == 'p') {
                fputs(line, stdout);
            } else {
                fputs(line, stdout);
            }
        }
        if (fi > 0)
            fclose(in);
        in = stdin;
    }
    return 0;
}
