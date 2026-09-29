/* fdisk.c - 内置 MBR 分区表创建器（无外部依赖）。
 * 用法: fdisk <device> [part-start-MiB] [part-end-MiB]
 *   例如: fdisk /dev/vda 1 64
 *
 * 在块设备偏移 0 处写一个 MBR:
 *   - 分区表:1 个主分区,从 start 到 end(含)
 *   - boot code:一个最小的 "跳转过去执行第一分区里的 vmlinuz 引导" 的
 *     stub,写 0x0000 偏移 0x1BE 处
 *   - 魔数 0x55AA
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define MBR_SECTOR 512

/* 分区条目 */
struct partition {
    unsigned char type;
    unsigned char start_head;
    unsigned char start_chs;     /* cyl+head 低 2 位 */
    unsigned int  start_lba;
    unsigned int  nsectors;
};

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: fdisk <device> [start-mib] [end-mib]\n");
        return 1;
    }
    const char *dev = argv[1];
    int start_mib = (argc > 2) ? atoi(argv[2]) : 1;
    int end_mib   = (argc > 3) ? atoi(argv[3]) : 0;

    int fd = open(dev, O_WRONLY);
    if (fd < 0) {
        perror(dev);
        return 1;
    }

    /* 用 BLKGETSIZE 拿设备大小 */
    unsigned long total_bytes = 0;
    if (ioctl(fd, 0x80041268 /* BLKGETSIZE64 */, &total_bytes) < 0) {
        fprintf(stderr, "fdisk: cannot get device size of %s\n", dev);
        close(fd);
        return 1;
    }
    long total_sectors = total_bytes / MBR_SECTOR;

    if (end_mib <= 0)
        end_mib = (int)(total_bytes / (1024 * 1024));
    int start_lba = start_mib * 2048;        /* 1 MiB = 2048 扇区 */
    int end_lba   = end_mib * 2048 - 1;
    if (end_lba >= total_sectors)
        end_lba = total_sectors - 1;
    if (start_lba > end_lba) {
        fprintf(stderr, "fdisk: invalid range\n");
        close(fd);
        return 1;
    }

    unsigned char mbr[MBR_SECTOR];
    memset(mbr, 0, sizeof mbr);

    /* 分区表 @ 0x1BE */
    unsigned char *pt = mbr + 0x1BE;
    /* type: 0x83 (Linux ext4) */
    pt[0] = 0x83;
    /* CHS start: use 255/63/63 to mean "use LBA" */
    pt[1] = 0xFF;
    pt[2] = (unsigned char)((start_lba >> 11) & 0xFF);
    pt[3] = (unsigned char)((start_lba & 0x1FF));
    /* LBA start */
    pt[4] = (unsigned char)(start_lba & 0xFF);
    pt[5] = (unsigned char)((start_lba >> 8) & 0xFF);
    pt[6] = (unsigned char)((start_lba >> 16) & 0xFF);
    pt[7] = (unsigned char)((start_lba >> 24) & 0xFF);
    /* sector count */
    unsigned int nsec = end_lba - start_lba + 1;
    if (nsec > 0xFFFFFFFFu) nsec = 0xFFFFFFFFu;
    pt[8] = (unsigned char)(nsec & 0xFF);
    pt[9] = (unsigned char)((nsec >> 8) & 0xFF);
    pt[10] = (unsigned char)((nsec >> 16) & 0xFF);
    pt[11] = (unsigned char)((nsec >> 24) & 0xFF);
    /* CHS end: 255/63/63 */
    pt[12] = 0xFF;
    pt[13] = 0xFF;
    pt[14] = 0xFF;

    /* boot code: 跳到 0x1FE (魔数位置前),再跳 0x200 (系统区)
     * 实际安装时,install 程序会把 vmlinuz 写到第一分区的引导扇区,
     * 这里只写一个占位 jmp,真正的引导由 GRUB2 或自写 boot 完成 */
    mbr[0]  = 0xEB;
    mbr[1]  = 0x58;          /* jmp +0x58 -> 0x5A, 到魔数 */
    mbr[2]  = 0x90;

    /* 魔数 */
    mbr[MBR_SECTOR - 2] = 0x55;
    mbr[MBR_SECTOR - 1] = 0xAA;

    if (pwrite(fd, mbr, MBR_SECTOR, 0) != MBR_SECTOR) {
        perror("fdisk: write MBR");
        close(fd);
        return 1;
    }
    fsync(fd);
    close(fd);

    printf("fdisk: created partition 1 on %s: LBA %d .. %d (%u sectors, type 0x83)\n",
           dev, start_lba, end_lba, nsec);
    return 0;
}
