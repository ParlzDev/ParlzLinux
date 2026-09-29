/* head.c - 取文件前 N 行。用法: head [-n <N>] [file...]
 * 无文件读 stdin;默认 10 行(与系统 head 一致)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        int c = 0;
        char line[4096];
        while (c++ < n && fgets(line, sizeof line, stdin))
            fputs(line, stdout);
        return 0;
    }
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "r");
        if (!f) {
            perror(argv[i]);
            continue;
        }
        if (i > 1)
            printf("==> %s <==\n", argv[i]);
        int c = 0;
        char line[4096];
        while (c++ < n && fgets(line, sizeof line, f))
            fputs(line, stdout);
        fclose(f);
    }
    return 0;
}
