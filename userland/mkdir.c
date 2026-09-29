/* mkdir.c - 创建目录。用法: mkdir [-p] <dir>... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* -p:逐级创建缺失的父目录;任一级失败(非 EEXIST)即报错 */
static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    char *p;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof tmp) {
        errno = EINVAL;
        return -1;
    }
    memcpy(tmp, path, len + 1);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;              /* 逐级建父目录 */
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    /* 参数解析:支持 -p 在任意位置,也支持 mkdir a b(无 -p) */
    int i;
    int pflag = 0;
    char *dirs[32];
    int ndirs = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            pflag = 1;
        } else if (strncmp(argv[i], "-", 1) == 0) {
            fprintf(stderr, "mkdir: invalid option -- '%c'\n",
                    argv[i][1] ? argv[i][1] : '?');
            fprintf(stderr, "usage: mkdir [-p] <dir>...\n");
            return 1;
        } else {
            if (ndirs < 31)
                dirs[ndirs++] = argv[i];
        }
    }
    if (ndirs == 0) {
        fprintf(stderr, "usage: mkdir [-p] <dir>...\n");
        return 1;
    }
    int rc = 0;
    for (i = 0; i < ndirs; i++) {
        if (pflag ? mkdir_p(dirs[i]) :
                    (mkdir(dirs[i], 0755) < 0 && errno != EEXIST)) {
            perror(dirs[i]);
            rc = 1;
        }
    }
    return rc;
}
