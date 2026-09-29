/* which.c - 在 PATH 里找命令, 打印第一个命中路径。
 * 用法: which <cmd> [cmd2 ...]
 * 逐段搜 PATH, 命中即打印 <段>/<cmd>, 多命令每个一行。
 * 命中判据: access(full, X_OK)==0 且 stat 成功 —— 悬空软链(目标已删,
 * 如 pm 卸载工具链包后残留的 /bin/gcc)access 返回 ENOENT 不命中,
 * 故卸载后 which 报 not found(与宿主 which 对悬空软链的语义一致)。
 * 找不到打印 "which: <cmd>: not found" 到 stderr, 返回 1。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: which CMD...\n");
        return 2;
    }
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/usr/sbin:/bin:/sbin";

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *save = NULL;
        char *copy = strdup(path);
        if (!copy) {
            fprintf(stderr, "which: %s: OOM\n", argv[i]);
            rc = 1;
            continue;
        }
        int found = 0;
        char *tok = strtok_r(copy, ":", &save);
        while (tok) {
            char full[4096];
            snprintf(full, sizeof full, "%s/%s", tok, argv[i]);
            struct stat st;
            /* access(X_OK) 判定软链终指向可执行; stat 成功排除悬空软链 */
            if (access(full, X_OK) == 0 && stat(full, &st) == 0) {
                printf("%s\n", full);
                found = 1;
                break;
            }
            tok = strtok_r(NULL, ":", &save);
        }
        free(copy);
        if (!found) {
            fprintf(stderr, "which: %s: not found\n", argv[i]);
            rc = 1;
        }
    }
    return rc;
}
