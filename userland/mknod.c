/* mknod.c - 创建设备节点(简化版 mknod 命令)。
 * 用法: mknod <path> <maj> <min>
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "usage: mknod <path> <maj> <min>\n");
        return 1;
    }
    int maj = atoi(argv[2]), min = atoi(argv[3]);
    if (mknod(argv[1], S_IFBLK | 0600, (maj << 8) | min) < 0) {
        perror("mknod");
        return 1;
    }
    chmod(argv[1], 0666);
    printf("mknod %s (blk %d:%d)\n", argv[1], maj, min);
    return 0;
}
