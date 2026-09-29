/* mv.c - 移动/重命名文件。用法: mv <src> <dst>
 * dst 是已存在目录 → 移入(等价 mv src dst/);否则 rename。
 * 跨文件系统(不同 mount)时退化:cp + rm(Parlz 无跨设备 move)。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "usage: mv <src> <dst>\n");
        return 1;
    }
    const char *src = argv[1], *dst = argv[2];
    struct stat st;

    if (rename(src, dst) == 0)
        return 0;
    int err = errno;
    /* 同设备失败通常是 ENOSPC/其它;跨设备 EXDEV 需 cp + rm */
    if (err == EXDEV) {
        /* dst 是目录则拼 src 名 */
        char target[512];
        if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
            const char *base = strrchr(src, '/');
            snprintf(target, sizeof target, "%s/%s", dst, base ? base + 1 : src);
        } else {
            snprintf(target, sizeof target, "%s", dst);
        }
        /* 简易 cp:逐字节 */
        int in = open(src, O_RDONLY);
        if (in < 0) { perror(src); return 1; }
        int out = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { close(in); perror(target); return 1; }
        char buf[65536];
        ssize_t n;
        while ((n = read(in, buf, sizeof buf)) > 0) {
            if (write(out, buf, n) != n) {
                int e2 = errno;
                close(in); close(out);
                fprintf(stderr, "mv: write %s: %s\n", target, strerror(e2));
                return 1;
            }
        }
        close(in);
        close(out);
        unlink(src);
        return 0;
    }
    fprintf(stderr, "mv: rename %s -> %s: %s\n", src, dst, strerror(err));
    return 1;
}
