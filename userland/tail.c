/* tail.c - 取文件末尾 N 行。用法: tail [-n <N>] [file...]
 * 无文件读 stdin;默认 10 行(与系统 tail 一致)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 读 stdin 全部行,打印末尾 n 行 */
static void tail_stdin(int n)
{
    char **all = NULL;
    size_t an = 0, acap = 0;
    char tmp[4096];
    while (fgets(tmp, sizeof tmp, stdin)) {
        if (an == acap) {
            acap = acap ? acap * 2 : 64;
            all = (char **)realloc(all, acap * sizeof *all);
        }
        all[an++] = strdup(tmp);
    }
    size_t start = (an < (size_t)n) ? 0 : an - n;
    for (size_t k = start; k < an; k++)
        fputs(all[k], stdout);
    for (size_t k = 0; k < an; k++)
        free(all[k]);
    free(all);
}

/* 读单个文件,打印末尾 n 行 */
static int tail_file(const char *path, int n)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return 1;
    }
    char **all = NULL;
    size_t an = 0, acap = 0;
    char tmp[4096];
    while (fgets(tmp, sizeof tmp, f)) {
        if (an == acap) {
            acap = acap ? acap * 2 : 64;
            all = (char **)realloc(all, acap * sizeof *all);
        }
        all[an++] = strdup(tmp);
    }
    fclose(f);
    size_t start = (an < (size_t)n) ? 0 : an - n;
    for (size_t k = start; k < an; k++)
        fputs(all[k], stdout);
    for (size_t k = 0; k < an; k++)
        free(all[k]);
    free(all);
    return 0;
}

int main(int argc, char *argv[])
{
    int n = 10;
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) {
        n = atoi(argv[++i]);
        i++;
    }
    if (n <= 0)
        n = 10;

    if (i >= argc) {
        tail_stdin(n);
        return 0;
    }
    for (; i < argc; i++) {
        if (i > 1)
            printf("==> %s <==\n", argv[i]);
        tail_file(argv[i], n);
    }
    return 0;
}
