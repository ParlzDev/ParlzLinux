/* wc.c - 统计行数/单词数/字节数。用法: wc [-l] [-w] [-c] [file...]
 * 无文件时读 stdin; -l 只计行, -w 只计词, -c 只计字节(默认全给)。
 * 面向管道/最小 shell:行数以 \n 计,词数以空白分隔。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <string.h>
#include <ctype.h>

int main(int argc, char *argv[])
{
    int opt_l = 0, opt_w = 0, opt_c = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        for (char *f = argv[i] + 1; *f; f++) {
            if (*f == 'l') opt_l = 1;
            else if (*f == 'w') opt_w = 1;
            else if (*f == 'c') opt_c = 1;
        }
        i++;
    }
    int none = !(opt_l || opt_w || opt_c);
    if (none)
        opt_l = opt_w = opt_c = 1;

    int nfiles = 0;
    for (; i < argc; i++)
        nfiles++;

    FILE *f = stdin;
    int multi = nfiles > 1;
    for (int fi = 0; fi <= nfiles; fi++) {
        if (fi > 0) {
            const char *path = argv[fi];
            f = fopen(path, "rb");
            if (!f) {
                perror(path);
                continue;
            }
        }
        long lines = 0, words = 0, bytes = 0;
        int in_word = 0;
        int c;
        while ((c = fgetc(f)) != EOF) {
            bytes++;
            if (c == '\n')
                lines++;
            if (isspace(c))
                in_word = 0;
            else if (!in_word)
                in_word = 1, words++;
        }
        /* 末行无 \n 也算一行(与 wc 一致:行数以输入末尾) */
        if (bytes > 0 && (c != '\n'))
            lines++;
        if (fi > 0)
            fclose(f);

        char label[256] = "";
        if (multi && fi > 0)
            snprintf(label, sizeof label, " %s", argv[fi]);
        else if (fi == 0 && multi)
            snprintf(label, sizeof label, " -");

        if (none || (opt_l && opt_w && opt_c)) {
            printf("%9ld %9ld %9ld%s\n", lines, words, bytes, label);
        } else {
            /* 单项:只显示选中的列 */
            char out[64] = "";
            int w = 0;
            if (opt_l) w += snprintf(out + w, sizeof out - w, "%9ld ", lines);
            if (opt_w) w += snprintf(out + w, sizeof out - w, "%9ld ", words);
            if (opt_c) w += snprintf(out + w, sizeof out - w, "%9ld", bytes);
            printf("%s%s\n", out, label);
        }
        f = stdin;
    }
    return 0;
}
