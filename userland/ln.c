/* ln.c - 创建符号链接。用法: ln -s <target> <link>
 * 无 -s 时报错(硬链接在 initramfs 无意义,只实现 -s)。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    int s = 0;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-s"))
            s = 1;
        i++;
    }
    if (!s || i + 1 >= argc) {
        fprintf(stderr, "usage: ln -s <target> <link>\n");
        return 1;
    }
    if (symlink(argv[i], argv[i + 1]) < 0) {
        perror("ln");
        return 1;
    }
    return 0;
}
