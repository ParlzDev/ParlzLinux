/* sort.c - 对输入行排序。用法: sort [-r] [-n] [file...]
 * 无文件时读 stdin; -r 逆序, -n 按数值比较(缺省按字典序)。
 * 面向最小 shell:整行缓存后 qsort。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}
static int cmp_num(const void *a, const void *b)
{
    return (int)(strtod(*(const char **)a, NULL) -
                 strtod(*(const char **)b, NULL));
}

int main(int argc, char *argv[])
{
    int rev = 0, numeric = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        for (char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r') rev = 1;
            else if (*f == 'n') numeric = 1;
        }
        i++;
    }
    FILE *in = stdin;
    if (i < argc) {
        in = fopen(argv[i], "r");
        if (!in) {
            perror(argv[i]);
            return 1;
        }
    }
    char *lines[4096];
    int n = 0;
    char buf[8192];
    while (fgets(buf, sizeof buf, in) && n < 4096) {
        buf[strcspn(buf, "\n")] = 0;
        char *c = strdup(buf);
        if (c)
            lines[n++] = c;
    }
    int (*cmp)(const void *, const void *) =
        numeric ? cmp_num : cmp_str;
    qsort(lines, n, sizeof lines[0], cmp);
    for (int k = 0; k < n; k++)
        if (rev)
            ;
        else
            puts(lines[k]);
    if (rev)
        for (int k = n - 1; k >= 0; k--)
            puts(lines[k]);
    if (in != stdin)
        fclose(in);
    return 0;
}
