/* mount.c - 简易 mount(2) 包装。
 * 用法:
 *   mount [-t type] [-o opts] <src> <dst>
 *   mount <src> <dst> <type> [data]         (位置式, 兼容旧脚本)
 * 两种写法都支持 —— 之前只认位置式, 而启动脚本写的是标准的
 * `mount -t ext4 /dev/vda2 /mnt`, 于是 src 被当成 "-t", mount(2) 必然
 * 失败(还被 2>/dev/null 吞掉), 现象是"分区写好了却挂不上, 停在
 * initramfs"。
 *
 * -o 支持逗号分隔, 识别 ro/rw/noatime/nodiratime/nosuid/noexec/
 * nodev/sync/remount(映射到 mount(2) flags), 其余原样作为 data 传下去
 * (如 mode=1777 这类文件系统私有选项)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>

static void parse_opts(const char *opts, unsigned long *flags, char *data,
                       size_t dlen)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s", opts ? opts : "");
    data[0] = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        if (!strcmp(tok, "ro"))              *flags |= MS_RDONLY;
        else if (!strcmp(tok, "rw"))         *flags &= ~(unsigned long)MS_RDONLY;
        else if (!strcmp(tok, "noatime"))    *flags |= MS_NOATIME;
        else if (!strcmp(tok, "nodiratime")) *flags |= MS_NODIRATIME;
        else if (!strcmp(tok, "nosuid"))     *flags |= MS_NOSUID;
        else if (!strcmp(tok, "noexec"))     *flags |= MS_NOEXEC;
        else if (!strcmp(tok, "nodev"))      *flags |= MS_NODEV;
        else if (!strcmp(tok, "sync"))       *flags |= MS_SYNCHRONOUS;
        else if (!strcmp(tok, "remount"))    *flags |= MS_REMOUNT;
        else {
            if (data[0])
                strncat(data, ",", dlen - strlen(data) - 1);
            strncat(data, tok, dlen - strlen(data) - 1);
        }
    }
}

int main(int argc, char *argv[])
{
    const char *src = NULL, *dst = NULL, *type = NULL, *opts = NULL;
    const char *data = NULL;
    unsigned long flags = 0;
    int i = 1;

    /* 先扫选项: -t type / -o opts / -r(只读) */
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            type = argv[++i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            opts = argv[++i];
        } else if (!strcmp(argv[i], "-r")) {
            flags |= MS_RDONLY;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "mount: 不支持的选项 %s\n", argv[i]);
            return 1;
        } else {
            break;
        }
    }
    /* 位置参数: src dst [type [data]] */
    if (i < argc) src = argv[i++];
    if (i < argc) dst = argv[i++];
    if (i < argc && !type) type = argv[i++];
    if (i < argc) data = argv[i++];

    if (!src || !dst) {
        fprintf(stderr, "usage: mount [-t type] [-o opts] <src> <dst>\n"
                        "       mount <src> <dst> <type> [data]\n");
        return 1;
    }
    char odata[512];
    odata[0] = 0;
    if (opts)
        parse_opts(opts, &flags, odata, sizeof odata);
    if (data && data[0]) {
        /* 位置式 data 与 -o 私有选项合并 */
        if (odata[0])
            strncat(odata, ",", sizeof odata - strlen(odata) - 1);
        strncat(odata, data, sizeof odata - strlen(odata) - 1);
    }

    if (mount(src, dst, type, flags, odata[0] ? odata : NULL) < 0) {
        fprintf(stderr, "mount: %s -> %s (%s): %s\n", src, dst,
                type ? type : "auto", strerror(errno));
        return 1;
    }
    printf("mounted %s on %s (%s)\n", src, dst, type ? type : "auto");
    return 0;
}
