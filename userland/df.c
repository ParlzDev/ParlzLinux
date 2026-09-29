/* df.c - 列出已挂载文件系统的磁盘使用。用法: df
 * 读 /proc/mounts + statvfs 计算 used/available。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

int main(void)
{
    printf("%-28s %10s %10s %10s %s\n",
           "Filesystem", "1K-blocks", "used", "avail", "Mounted");
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        perror("/proc/mounts");
        return 1;
    }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        /* /proc/mounts 行格式: dev mnt fs type opts dump pass */
        char dev[128], mnt[256], typ[32], opts[64];
        int dump, pass;
        if (sscanf(line, "%127s %255s %31s %63s %d %d",
                   dev, mnt, typ, opts, &dump, &pass) < 3)
            continue;
        (void)opts; (void)dump; (void)pass;
        /* 跳过伪文件系统 */
        if (!strcmp(typ, "proc") || !strcmp(typ, "sysfs") ||
            !strcmp(typ, "devtmpfs") || !strcmp(typ, "tmpfs") ||
            !strcmp(typ, "cgroup2") || !strcmp(typ, "squashfs"))
            continue;
        struct statvfs v;
        if (statvfs(mnt, &v) != 0)
            continue;
        unsigned long bsize = v.f_bsize;
        unsigned long blocks = v.f_blocks * bsize / 1024;
        unsigned long free_  = v.f_bfree  * bsize / 1024;
        unsigned long avail  = v.f_bavail * bsize / 1024;
        unsigned long used   = (blocks > free_) ? blocks - free_ : 0;
        printf("%-28s %10lu %10lu %10lu %s\n",
               dev, blocks, used, avail, mnt);
    }
    fclose(f);
    return 0;
}
