// SPDX-License-Identifier: GPL-2.0-or-later
/* PPM 原样委托真正的 OPKG；不再读写损坏的旧 .ppm 自定义归档。
 * opkg.c 复用此入口，仅去掉 PPM 专用子命令。
 * 后端固定为同目录 opkg-native，不从 PATH/环境变量查找，避免递归。
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef PARLZ_OPKG_ENTRY
#define PARLZ_OPKG_ENTRY 0
#endif

int main(int argc, char **argv)
{
    char backend[PATH_MAX];
    ssize_t n;
    char *slash;
    const char *name = PARLZ_OPKG_ENTRY ? "opkg" : "ppm";

    if (!PARLZ_OPKG_ENTRY && argc > 1) {
        if (!strcmp(argv[1], "make") || !strncmp(argv[1], "legacy-", 7)) {
            fprintf(stderr, "ppm: 旧 .ppm 格式已停用，旧代码仅保留备份；请制作标准 .ipk 包。\n");
            return 2;
        }
        if (!strcmp(argv[1], "opkg")) {
            argv++;
            argc--;
        }
    }
    (void)argc;
    n = readlink("/proc/self/exe", backend, sizeof backend - 1);
    if (n < 0) {
        /* /proc 尚未挂载时，使用系统安装位置。 */
        strcpy(backend, "/bin/opkg-native");
    } else {
        if ((size_t)n >= sizeof backend - 1) {
            fprintf(stderr, "%s: 后端路径过长\n", name);
            return 126;
        }
        backend[n] = '\0';
        slash = strrchr(backend, '/');
        if (!slash || (size_t)(slash + 1 - backend) + sizeof "opkg-native" > sizeof backend) {
            fprintf(stderr, "%s: 无法确定后端路径\n", name);
            return 126;
        }
        strcpy(slash + 1, "opkg-native");
    }
    argv[0] = backend;
    execv(backend, argv);
    /* execv 成功后 PID、信号、退出码均由 OPKG 原生保留。 */
    int err = errno;
    fprintf(stderr, "%s: 无法执行真正的 OPKG 后端 %s: %s\n", name, backend, strerror(err));
    return err == ENOENT ? 127 : 126;
}
