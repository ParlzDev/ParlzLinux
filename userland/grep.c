/* grep.c - 按模式过滤行。用法: grep <pattern> [file...]
 * 无文件时读 stdin;固定字符串匹配(非正则,极简,面向最小 shell)。
 * 支持 -v(反选)、-n(带行号)、-c(只计匹配行)。
 */
#include <stdio.h>
#include <string.h>

/* 过滤一个流; 返回**选中行数**(调用方据此定退出码: 没选中 -> 1) */
static int do_stream(FILE *f, const char *name, const char *pat,
                     int inv, int show_no, int count, int multi)
{
    int lineno = 0, matched = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        lineno++;
        int hit = (strstr(line, pat) != NULL);
        if (hit == inv)
            continue;
        matched++;
        if (count)
            continue;
        if (multi)
            printf("%s:", name);
        if (show_no)
            printf("%d:", lineno);
        /* strcspn 已把换行剥掉,统一补回(与 GNU grep 一致,行尾必有换行) */
        fwrite(line, 1, strcspn(line, "\n"), stdout);
        putchar('\n');
    }
    if (count) {
        /* 单个文件/stdin 只打数字; 多文件才带文件名前缀(与 GNU grep 一致) */
        if (multi && name)
            printf("%s:%d\n", name, matched);
        else
            printf("%d\n", matched);
    }
    return matched;
}

int main(int argc, char *argv[])
{
    int inv = 0, show_no = 0, count = 0;
    int i = 1;
    const char *pat = NULL;
    while (i < argc && argv[i][0] == '-') {
        for (char *f = argv[i] + 1; *f; f++) {
            if (*f == 'v') inv = 1;
            else if (*f == 'n') show_no = 1;
            else if (*f == 'c') count = 1;
        }
        i++;
    }
    if (i < argc)
        pat = argv[i++];
    if (!pat) {
        fprintf(stderr, "usage: grep [-v] [-n] [-c] <pattern> [file...]\n");
        return 2;
    }

    int nfiles = argc - i;
    int multi = nfiles > 1;

    /* 无文件读 stdin;有文件只处理列出的文件。旧代码三处叠加 bug:
     * nfiles 计数死循环、有文件仍先读一遍 stdin(管道无 EOF 即挂死)、
     * 文件名取 argv[i-1] 且 i 不再前进(实际打开的是模式参数)。 */
    int selected = 0, errs = 0;
    if (nfiles == 0) {
        selected = do_stream(stdin, NULL, pat, inv, show_no, count, multi);
    } else {
        for (; i < argc; i++) {
            FILE *f = fopen(argv[i], "r");
            if (!f) {
                perror(argv[i]);
                errs = 1;
                continue;
            }
            selected += do_stream(f, argv[i], pat, inv, show_no, count, multi);
            fclose(f);
        }
    }
    /* 退出码: 0 = 有选中行; 1 = 一行都没选中; 2 = 出错(与 GNU grep 一致) */
    if (errs)
        return 2;
    return selected ? 0 : 1;
}
