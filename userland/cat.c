/* cat.c - 拼接输出文件。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 把 f 全部写到 stdout;0 成功 */
static int pump(FILE *f)
{
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        if (write(STDOUT_FILENO, buf, n) < 0)
            return 1;
    return 0;
}

int main(int argc, char *argv[])
{
    int i;
    /* 无参数 = 读 **stdin**(POSIX 把操作数缺省当 "-")。
     * 老实现直接进下面的 for 循环, 一个参数都没有就什么都不做 ——
     * 于是 `cat < f`、`... | cat`、`cat | grep x` 这类用法全是空输出(实测)。 */
    if (argc == 1)
        return pump(stdin);
    for (i = 1; i < argc; i++) {
        FILE *f = strcmp(argv[i], "-") ? fopen(argv[i], "rb") : stdin;
        if (!f) {
            perror(argv[i]);
            return 1;
        }
        if (pump(f))
            return 1;
        if (f != stdin)
            fclose(f);
    }
    return 0;
}
