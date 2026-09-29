/* boot.c - 简单的重启 + halt 命令。
 * 用法: reboot | halt | poweroff
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/reboot.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: reboot | halt | poweroff\n");
        return 1;
    }
    if (!strcmp(argv[1], "reboot")) {
        printf("rebooting...\n");
        sync();
        reboot(RB_AUTOBOOT);
        _exit(0);
    } else if (!strcmp(argv[1], "halt")) {
        printf("halting...\n");
        sync();
        reboot(RB_HALT_SYSTEM);
        _exit(0);
    } else if (!strcmp(argv[1], "poweroff")) {
        printf("power off...\n");
        sync();
        reboot(RB_POWER_OFF);
        _exit(0);
    }
    fprintf(stderr, "unknown command: %s\n", argv[1]);
    return 1;
}
