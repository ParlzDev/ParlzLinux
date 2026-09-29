/* rm.c - 删除文件或目录。用法: rm [-rf] <path>... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

/* 递归删除目录 */
static int rm_dir_rec(const char *path)
{
    struct stat st;
    DIR *d = opendir(path);
    struct dirent *e;
    char sub[1024];
    if (!d) {
        /* 空目录或普通文件:直接 unlink */
        if (unlink(path) < 0 && errno != ENOENT) {
            perror(path);
            return -1;
        }
        return 0;
    }
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (rm_dir_rec(sub) < 0) {
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    /* 目录现在为空了,删掉它 */
    if (rmdir(path) < 0 && errno != ENOENT) {
        perror(path);
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int i = 1;
    int recursive = 0, force = 0;
    char opts[32];
    while (argv[i] && argv[i][0] == '-' && argv[i][1]) {
        /* 拷"-"后面的选项字母。老实现是 `opts[0] = 0; o = 1;` ——
         * 先把 opts[0] 写成 NUL 再**从 1 开始写**字母, 于是 opts 永远是空串,
         * strchr 找不到任何标志: `rm -rf` 既没递归也没 force(实测:
         * `rm -f 不存在的文件` 返回 1、`rm -r 目录` 直接拒绝)。 */
        snprintf(opts, sizeof opts, "%s", argv[i] + 1);
        if (strchr(opts, 'r') || strchr(opts, 'R'))
            recursive = 1;
        if (strchr(opts, 'f'))
            force = 1;
        i++;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: rm [-rf] <path>...\n");
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        struct stat st;
        int is_dir = 0;
        if (stat(argv[i], &st) < 0) {
            if (force)
                continue;
            perror(argv[i]);
            rc = 1;
            continue;
        }
        is_dir = S_ISDIR(st.st_mode);
        int err;
        if (is_dir && !recursive) {
            if (!force) {
                fprintf(stderr, "rm: %s: is a directory (use -r)\n", argv[i]);
                rc = 1;
                continue;
            }
            err = rmdir(argv[i]);
        } else if (is_dir) {
            err = rm_dir_rec(argv[i]);
        } else {
            err = unlink(argv[i]);
        }
        if (err < 0 && errno != ENOENT) {
            perror(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
