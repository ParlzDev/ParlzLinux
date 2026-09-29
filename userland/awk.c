/* awk.c - 极简 awk(面向最小 shell + 管道)。
 * 用法: awk <prog> [file...]
 * 支持的 prog 形式(子集,够用即可):
 *   { print }            打印每行
 *   { print $N }         打印第 N 列(按空白分隔)
 *   /pat/{ print }       含 pat 的行打印
 * 面向管道场景:默认打印整行;$N 列抽取是主要用例。
 * 无文件时读 stdin。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 跑一个输入流(awk 的动作子集)。 */
static void run_stream(FILE *in, const char *pat, int have_pat,
                       const int *colnums, int ncol)
{
    char line[8192];
    while (fgets(line, sizeof line, in)) {
        /* 模式过滤 */
        if (have_pat && pat[0] && !strstr(line, pat))
            continue;
        if (ncol > 0) {
            char save[8192];
            strcpy(save, line);
            char *savep = save;
            /* 分隔符要含 \r\n: 少了换行, 字段就会把行尾的 '\n' 一起带出去,
             * 每个字段后面多打一个空行(实测: `awk '{print $1}'` 输出两倍行距) */
            char *tok = strtok_r(save, " \t\r\n", &savep);
            int idx = 1;
            for (int k = 0; k < ncol; k++) {
                while (idx < colnums[k] && tok) {
                    tok = strtok_r(NULL, " \t\r\n", &savep);
                    idx++;
                }
                if (tok) {
                    if (k > 0)
                        putchar(' ');
                    fputs(tok, stdout);
                }
                idx = colnums[k] + 1;
                tok = strtok_r(NULL, " \t\r\n", &savep);
            }
            putchar('\n');
        } else {
            fputs(line, stdout);
        }
    }
}

int main(int argc, char *argv[])
{
    /* argc 只要求有 prog —— **没文件时读 stdin**(POSIX, 也是管道里最常见的
     * 用法:`... | awk '{print $1}'`)。老实现写的是 `argc < 3` 直接打 usage,
     * 等于"管道里用不了 awk"(实测), 与文件头注释的承诺也相反。 */
    if (argc < 2) {
        fprintf(stderr, "usage: awk '<prog>' [file...]\n");
        return 1;
    }
    const char *prog = argv[1];

    /* 解析 prog:模式 = prog 中首个 '/' 与下个 '/' 之间的字面串;
     * 动作 = '{' 与 '}' 之间,print $N 或 print。 */
    char pat[256] = "";
    char action[256] = "";
    int have_pat = 0;
    const char *sl = strchr(prog, '/');
    const char *sr = sl ? strchr(sl + 1, '/') : NULL;
    if (sl && sr) {
        size_t pl = (size_t)(sr - sl - 1);
        if (pl < sizeof pat - 1) {
            memcpy(pat, sl + 1, pl);
            pat[pl] = 0;
            have_pat = 1;
        }
    }
    const char *ob = strchr(prog, '{');
    const char *cb = ob ? strchr(ob + 1, '}') : NULL;
    if (ob && cb) {
        size_t al = (size_t)(cb - ob - 1);
        if (al < sizeof action - 1) {
            memcpy(action, ob + 1, al);
            action[al] = 0;
        }
    }

    /* 动作形式: "print" / "print $N" / "print $N $M ..." */
    int colnums[32];
    int ncol = 0;
    int just_print = 0;
    {
        char *a = strdup(action);
        if (a) {
            char *w = a;
            while (*w == ' ')
                w++;
            if (!strncmp(w, "print", 5)) {
                w += 5;
                char *tok = w;
                while (tok && *tok && ncol < 32) {
                    while (*tok == ' ' || *tok == '\t')
                        tok++;
                    if (*tok == '$') {
                        tok++;
                        int n = 0;
                        while (*tok >= '0' && *tok <= '9')
                            n = n * 10 + (*tok++ - '0');
                        if (n > 0)
                            colnums[ncol++] = n;
                        just_print = 1;
                    } else if (*tok) {
                        while (*tok && *tok != ' ' && *tok != '\t')
                            tok++;
                        just_print = 1;
                    }
                }
            }
            free(a);
        }
    }

    if (argc == 2) {
        run_stream(stdin, pat, have_pat, colnums, ncol);
    } else {
        for (int i = 2; i < argc; i++) {
            FILE *f = fopen(argv[i], "r");
            if (!f) {
                perror(argv[i]);
                return 1;
            }
            run_stream(f, pat, have_pat, colnums, ncol);
            fclose(f);
        }
    }
    (void)just_print;
    return 0;
}
