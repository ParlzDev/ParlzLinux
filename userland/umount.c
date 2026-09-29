/* umount.c - 简易 umount(2) 包装：umount <dst> */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: umount <dst>\n");
        return 1;
    }
    if (umount2(argv[1], MNT_DETACH) < 0) {
        perror("umount");
        return 1;
    }
    printf("unmounted %s\n", argv[1]);
    return 0;
}
