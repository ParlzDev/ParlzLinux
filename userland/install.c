/* install.c - Parlz 命令行安装器(syslinux 方案)。
 * 用法: install [disk-device]
 *   无参数时自动从 /sys/block 探测第一个可安装磁盘(virtio/sd/hd,
 *   排除 CD-ROM/软驱/只读/空设备)。
 *
 * 引导链(两条,共用同一 FAT16 引导分区):
 *   Legacy(SeaBIOS): BIOS 读 LBA 0(syslinux mbr.bin)→ 找 0x06 活动分区
 *     → 读 /ldlinux.sys(syslinux BIOS 引导器)→ syslinux.cfg → linux /vmlinuz。
 *   UEFI(OVMF): 读 (fat) 分区的 /BOOTX64.EFI(syslinux.efi PE 桩)
 *     → syslinux.cfg → KERNEL /vmlinuz。
 * vmlinuz 内嵌 initramfs(rootfs 平铺 + shell + install),自带根。
 *
 * 步骤:
 *   1. 写 MBR:LBA 0 = 自写 INT13 0x50 引导码 + 双分区表(boot FAT16 + root)
 *   2. 分区 1(FAT16,40 MiB):整体写 gen-fatboot.sh 生成的引导镜像
 *      images/parlz-bootfat.img(ldlinux.sys + vmlinuz + bootx64.efi +
 *      syslinux.cfg),运行时从 ISO/附加盘/文件读入(见 open_boot_image)
 *   3. 分区 2(root):mkfs ext2 + cpfs 拷入当前根
 *   4. 写 install-done 标记(LBA 1,MBR 与 boot 分区之间的空闲区,
 *      与 root 分区不重叠,root 将来格式化也不会破坏标记)
 *   5. reboot 提示
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <dirent.h>

#define MBR_SECTOR 512
/* 分区 1:boot(FAT16)。LBA 2048(1 MiB 起)起 131072 扇区(64 MiB)。
 * 64 MiB @ 1 KiB 簇 = ~64760 簇, 在 FAT16 的 65524 上限内, 且能容纳
 * 内嵌 initramfs 的 vmlinuz(约 25~45 MB, 随用户空间增长)。
 * 旧 40 MiB 装不下 45 MB 的 vmlinuz(gen-fatboot cluster overflow)。
 * 起始在 1 MiB 对齐处(OVMF 识别 ESP 要求 >=1 MiB 对齐,8 MiB 的
 * 16384 在某些 OVMF 构建下不在其扫描窗口内)。
 * 引导链路(OVMF/UEFI):OVMF 识别 0xEF ESP → 读 /EFI/BOOT/BOOTX64.EFI
 * (= syslinux.efi PE 桩)→ 加载同目录 efi64 模块 → 读 /EFI/BOOT/syslinux.cfg
 * → KERNEL /vmlinuz + APPEND console。vmlinuz 内嵌 initramfs。
 * SeaBIOS(Legacy)走 LBA 0 的自写 INT13 0x50 引导码 → LBA 2048 载 VBR。
 * 分区 2:root,紧接 boot 区到盘尾(留作将来 root FS,暂不格式化)。 */
#define BOOT_PART_START_LBA 2048
#define BOOT_PART_SIZE_SECTORS 131072   /* 64 MiB(见上,与 gen-fatboot BOOT_SECTORS 同步) */
#define MBR_CODE_LEN 440      /* syslinux mbr.bin 长度 */

/* 注:write_mbr() 已改用自写 INT13 LBA 扩展读,syslinux_mbr 数组不再使用。
 * gen-syslinux-header.sh 仍生成 syslinux.h(供其他工具可选包含),
 * install.c 不再 include 它,避免 WSL 缺 /usr/lib/syslinux 时编译失败。 */

/* bootfat.h:host 侧 gen-fatboot.sh 生成的引导分区镜像**元数据**
 * (只有 BOOTFAT_IMAGE_BYTES; 镜像字节刻意不内嵌, 见下)。
 * 镜像本体 images/parlz-bootfat.img(64 MiB FAT16)内含:
 *   Legacy 路径: syslinux VBR + /ldlinux.sys + /ldlinux.c32 +
 *                /libcom32.c32 + /libutil.c32 + /syslinux.cfg + /vmlinuz
 *   UEFI 路径:   /EFI/BOOT/BOOTX64.EFI + efi64 模块 + syslinux.cfg
 * install 运行时从外部源读入镜像,整体 pwrite 到分区 1(LBA 2048)。
 *
 * ★ 为什么不内嵌: 镜像里有 vmlinuz,而 vmlinuz 里又嵌着 initramfs,
 * initramfs 里就有 install 自己 —— 内嵌会让 install 体积 = 镜像 =
 * vmlinuz = install ... 每轮构建涨一截(cluster overflow / 内核
 * do_populate_rootfs 写挂)。改为运行时读取,链条断开。
 * 外部源优先级(见 find_boot_image):
 *   1. 环境/内核对 cmdline 的 parlz.bootimg=<path|块设备>
 *   2. /boot/fat16.img(initramfs 或已装根里的镜像文件)
 *   3. /cdrom/boot/fat16.img(ISO 安装介质, 由 init 挂到 /cdrom)
 *   4. 自动扫描: 非目标盘、开头是 FAT16 引导扇区(EB ?? 90 + 55AA)的块设备
 *
 * mbrbin.h:build-userland.sh 从 /usr/lib/syslinux/mbr/mbr.bin 生成的
 * 440 字节 MBR 引导码(自搬运到 0x0600 再读 VBR, 见 write_mbr)。 */
#include "bootfat.h"
#include "mbrbin.h"

/* BLKPG ioctl 结构(用户态内联定义,内核 UAPI 头在 static userland 下不可用) */
#define BLKPG     0x1269      /* _IO(0x12, 105) */
#define BLKPG_ADD_PARTITION 1
struct blkpg_partition {
    long long start;
    long long length;
    int pno;
    char devname[64];
    char volname[64];
};
struct blkpg_ioctl_arg {
    int op;
    int flags;
    int datalen;
    void *data;
};
#ifndef BLKRRPART
#define BLKRRPART 0x1268      /* _IO(0x12, 128) */
#endif

/* 引导镜像源候选:环境变量/内核对 cmdline 的显式指定,再静态路径。
 * 静态路径覆盖几种常见放置:initramfs 内 /boot、ISO 挂载点 /cdrom 等。 */
static const char *boot_image_static_paths[] = {
    "/boot/fat16.img",
    "/boot/parlz-bootfat.img",
    "/cdrom/boot/fat16.img",
    "/mnt/cdrom/boot/fat16.img",
    "/media/cdrom/boot/fat16.img",
    "/mnt/boot/fat16.img",
    "/fat16.img",
    NULL
};

/* 扫 /proc/mounts: 任何 iso9660 挂载点下找 boot/fat16.img。
 * 用户手动 mount 了安装 ISO(或 init 挂到了非默认路径)也能找到。 */
static int find_boot_image_on_iso(char *out, size_t n)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char dev[256], mnt[256], type[64];
        if (sscanf(line, "%255s %255s %63s", dev, mnt, type) != 3)
            continue;
        if (strcmp(type, "iso9660") != 0)
            continue;
        char cand[600];
        snprintf(cand, sizeof cand, "%s/boot/fat16.img", mnt);
        FILE *t = fopen(cand, "rb");
        if (t) {
            fclose(t);
            snprintf(out, n, "%s", cand);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* 判断块设备/dev/xxx 开头是否为 FAT16 引导扇区(EB ?? 90 ... 55 AA)。
 * syslinux 的 VBR 是 EB 58 90, mkfs.vfat 的是 EB 3C 90 —— 只认跳转
 * 指令 + 魔数, 不锁死具体跳转距离, 两者都算命中。
 * 仅用于自动探测引导镜像源盘, 不命中即跳过, 不会误判普通数据盘。 */
static int is_fat16_vbr(const char *dev)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return 0;
    unsigned char b[512];
    ssize_t r = pread(fd, b, sizeof b, 0);
    int ok = 0;
    if (r >= 512) {
        unsigned char tail[2];
        if (pread(fd, tail, 2, 510) == 2 &&
            tail[0] == 0x55 && tail[1] == 0xAA &&
            b[0] == 0xEB && b[2] == 0x90)
            ok = 1;
    }
    close(fd);
    return ok;
}

static int ensure_dev_node(const char *devpath);

/* 找一个非目标盘的块设备作为引导镜像源。
 * 遍历 /sys/block(内核权威的"已认盘"清单)而不是 /dev —— /dev 里的节点由
 * devtmpfs 异步创建, 装盘时可能还没出来, 拿它当清单会漏盘。
 * 排除目标盘本身与其分区, 补建节点后按 FAT16 VBR 特征判定。 */
static int find_boot_image_dev(const char *target, char *out, size_t n)
{
    DIR *d = opendir("/sys/block");
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "vd", 2) != 0 &&
            strncmp(e->d_name, "sd", 2) != 0)
            continue;
        char full[128];
        snprintf(full, sizeof full, "/dev/%s", e->d_name);
        if (strcmp(full, target) == 0)
            continue;
        struct stat st;
        if (stat(full, &st) != 0) {
            if (ensure_dev_node(full) != 0)
                continue;
            if (stat(full, &st) != 0)
                continue;
        }
        if (!S_ISBLK(st.st_mode))
            continue;
        if (!is_fat16_vbr(full))
            continue;
        snprintf(out, n, "%s", full);
        found = 1;
        break;
    }
    closedir(d);
    return found;
}

/* devtmpfs 建 /dev/<name> 节点是**异步**的(内核已经认到盘, 节点还没出来,
 * 实测 install 会因此"找不到引导镜像")。从 /sys/block/<name>/dev 读
 * maj:min 自己 mknod 一个。只适用整盘(分区不在 /sys/block 顶层)。 */
static int ensure_dev_node(const char *devpath)
{
    const char *name = strrchr(devpath, '/');
    name = name ? name + 1 : devpath;
    char p[160];
    snprintf(p, sizeof p, "/sys/block/%s/dev", name);
    FILE *f = fopen(p, "r");
    if (!f)
        return -1;
    int maj = -1, min = 0;
    if (fscanf(f, "%d:%d", &maj, &min) != 2)
        maj = -1;
    fclose(f);
    if (maj < 0)
        return -1;
    if (mknod(devpath, S_IFBLK | 0660,
              ((maj & 0xfff) << 8) | (min & 0xff)) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* 列一遍当前块设备(容量/读写), 供"找不到引导镜像"时定位用 ——
 * 光说"没找到"没法定案: 到底是没挂第二块盘, 还是节点没出来,
 * 还是挂了但不是 FAT16。 */
static void report_block_devices(void)
{
    DIR *d = opendir("/sys/block");
    if (!d) {
        fprintf(stderr, "install:   (打不开 /sys/block)\n");
        return;
    }
    struct dirent *e;
    int n = 0;
    fprintf(stderr, "install:   当前块设备:");
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (strncmp(e->d_name, "loop", 4) == 0 ||
            strncmp(e->d_name, "ram", 3) == 0)
            continue;
        char p[160];
        snprintf(p, sizeof p, "/sys/block/%s/size", e->d_name);
        FILE *f = fopen(p, "r");
        unsigned long long sec = 0;
        if (f) {
            if (fscanf(f, "%llu", &sec) != 1)
                sec = 0;
            fclose(f);
        }
        snprintf(p, sizeof p, "/sys/block/%s/ro", e->d_name);
        f = fopen(p, "r");
        int ro = 0;
        if (f) {
            int v = 0;
            if (fscanf(f, "%d", &v) == 1)
                ro = v;
            fclose(f);
        }
        fprintf(stderr, " %s(%llu MiB%s)", e->d_name,
                sec / 2048ULL, ro ? ", 只读" : "");
        n++;
    }
    closedir(d);
    if (!n)
        fprintf(stderr, " (一个都没有)");
    fprintf(stderr, "\n");
}

/* 打开并校验引导镜像源,返回 fd 与总字节数。
 * 读不到任何源返回 -1 并在 stderr 说明可用的放置方式。 */
static int open_boot_image(const char *target, long *size_out)
{
    char path[256];
    const char *src = getenv("PARLZ_BOOT_IMG");
    int fd = -1;

    if (src && *src) {
        fd = open(src, O_RDONLY);
        if (fd < 0) {
            /* 节点可能只是 devtmpfs 还没来得及建 —— 自己补一个再试 */
            if (ensure_dev_node(src) == 0)
                fd = open(src, O_RDONLY);
        }
        if (fd >= 0)
            fprintf(stderr, "install: 引导镜像源 %s (PARLZ_BOOT_IMG)\n", src);
        else
            fprintf(stderr, "install: PARLZ_BOOT_IMG=%s 打不开: %s\n",
                    src, strerror(errno));
    }
    for (int i = 0; fd < 0 && boot_image_static_paths[i]; i++) {
        fd = open(boot_image_static_paths[i], O_RDONLY);
        if (fd >= 0)
            fprintf(stderr, "install: 引导镜像源 %s\n",
                    boot_image_static_paths[i]);
    }
    /* 已挂载的 ISO(挂载点任意)里的 boot/fat16.img */
    if (fd < 0 && find_boot_image_on_iso(path, sizeof path)) {
        fd = open(path, O_RDONLY);
        if (fd >= 0)
            fprintf(stderr, "install: 引导镜像源 %s (已挂 ISO)\n", path);
    }
    if (fd < 0) {
        if (find_boot_image_dev(target, path, sizeof path)) {
            fd = open(path, O_RDONLY);
            if (fd >= 0)
                fprintf(stderr, "install: 引导镜像源 %s (自动探测)\n", path);
        }
    }
    if (fd < 0) {
        fprintf(stderr,
            "install: 找不到引导镜像(即 FAT16 引导分区的内容)。\n"
            "  找过的顺序: $PARLZ_BOOT_IMG → /boot/fat16.img 等静态路径 →\n"
            "            已挂 ISO 里的 boot/fat16.img → 非目标盘的 FAT16 引导盘\n");
        report_block_devices();
        fprintf(stderr,
            "  引导镜像不入 initramfs(会自引用膨胀), 必须由外部提供:\n"
            "    - 最省事: 退出后用 scripts/boot-install.sh(或 Windows 侧 install.bat)\n"
            "      —— 它把 images/parlz-bootfat.img 挂成只读 /dev/vdb 并传\n"
            "        parlz.bootimg=/dev/vdb;scripts/boot.sh 同样会挂\n"
            "    - 或挂安装 ISO: mount -o ro -t iso9660 /dev/sr0 /cdrom\n"
            "      (镜像在 ISO 内的 /boot/fat16.img)\n"
            "    - 或自己加一块只读盘: -drive file=images/parlz-bootfat.img,if=virtio\n"
            "      并在 cmdline 加 parlz.bootimg=/dev/vdb\n"
            "    - 或把镜像放进当前根的 /boot/fat16.img\n");
        return -1;
    }
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) {
        fprintf(stderr, "install: 引导镜像源为空\n");
        close(fd);
        return -1;
    }
    *size_out = (long)sz;
    lseek(fd, 0, SEEK_SET);
    return fd;
}

/* 写 boot 分区:把引导镜像(40 MiB FAT16,含 ldlinux.sys + vmlinuz +
 * bootx64.efi + syslinux.cfg)整体 pwrite 到 BOOT_PART_START_LBA(=2048,
 * 与 MBR 分区表 P1 起点一致)。镜像不再内嵌在 install 里,运行时从
 * ISO / 附加盘 / 文件读入(见 open_boot_image)。
 *
 * 注意: 旧版曾写 LBA 16384 —— 分区 1 实际从 LBA 2048 起(见 MBR
 * 分区表与 BOOT_PART_START_LBA),16384 并非 P1 内的任何偏移,
 * 会导致 VBR 永远为空、SeaBIOS "Booting from Hard Disk.." 后静默。 */
static int write_boot_partition(const char *disk, int src, long total)
{
    if (total <= 0 || src < 0) {
        fprintf(stderr, "install: 内部错误: 引导镜像未就绪\n");
        return -1;
    }
    if (total > (long)BOOT_PART_SIZE_SECTORS * MBR_SECTOR) {
        fprintf(stderr,
            "install: 引导镜像 %ld 字节超出分区 1 容量 %ld 字节\n",
            total, (long)BOOT_PART_SIZE_SECTORS * MBR_SECTOR);
        return -1;
    }
    int fd = open(disk, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    long off = (long)BOOT_PART_START_LBA * MBR_SECTOR;
    long done = 0;
    char buf[65536];
    while (done < total) {
        long chunk = total - done;
        if (chunk > (long)sizeof buf)
            chunk = (long)sizeof buf;
        ssize_t r = read(src, buf, (size_t)chunk);
        if (r <= 0) {
            fprintf(stderr, "install: 读引导镜像失败(偏移 %ld): %s\n",
                    done, r == 0 ? "提前 EOF" : strerror(errno));
            close(fd);
            return -1;
        }
        if (pwrite(fd, buf, (size_t)r, off + done) != r) {
            perror("install: write boot partition");
            close(fd);
            return -1;
        }
        done += r;
    }
    /* 写后读回 VBR 复核(必须新开只读 fd —— fd 是 O_WRONLY, 在它上面
     * pread 会 EBADF 返回失败, 三个字节保持未初始化, 于是永远打印
     * MISSING 假警报。踩过: 明明写成功却报 "VBR MISSING")。
     * syslinux VBR = EB 58 90 + OEM "SYSLINUX"; mkfs.vfat 桩 = EB 3C 90。
     * 只校验 EB ?? 90 跳转 + 尾部 55 AA。 */
    {
        unsigned char head[16], tail[2];
        int rfd = open(disk, O_RDONLY);
        int ok = 0, sig = 0;
        memset(head, 0, sizeof head);
        memset(tail, 0, sizeof tail);
        if (rfd >= 0) {
            if (pread(rfd, head, sizeof head, off) == (ssize_t)sizeof head &&
                pread(rfd, tail, sizeof tail, off + 0x1FE) ==
                (ssize_t)sizeof tail &&
                head[0] == 0xEB && head[2] == 0x90 &&
                tail[0] == 0x55 && tail[1] == 0xAA)
                ok = 1;
            if (head[0] == 0xEB && head[2] == 0x90)
                sig = 1;
            close(rfd);
        }
        if (ok)
            printf("  VBR @LBA %d OK: EB%02x 90, 引导标志 55AA, syslinux %s\n",
                   BOOT_PART_START_LBA, head[1],
                   memcmp(head + 3, "SYSLINUX", 8) == 0 ? "VBR" : "(非 syslinux OEM)");
        else if (sig)
            printf("  VBR @LBA %d 跳转存在(EB%02x 90)但尾部非 55AA(%02x%02x)\n",
                   BOOT_PART_START_LBA, head[1], tail[0], tail[1]);
        else
            printf("  VBR @LBA %d MISSING(首 3 字节 %02x%02x%02x)\n",
                   BOOT_PART_START_LBA, head[0], head[1], head[2]);
    }
    fsync(fd);
    close(fd);
    return 0;
}

/* 写到整盘(diskpath)在磁盘上的 LBA 偏移 boot_start_lba 处。
 * 本内核分区不注册(BLKRRPART/BLKPG 无效),/dev/<disk>N 不可用,
 * 直接 pwrite 整盘 LBA 偏移。 */
static long write_raw_at(const char *src, const char *diskpath,
                         unsigned int boot_start_lba)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(diskpath, O_WRONLY);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    long total = 0;
    long off = (long)boot_start_lba * MBR_SECTOR;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        if (pwrite(out, buf, n, off) != n) {
            close(in); close(out);
            return -1;
        }
        off += n;
        total += n;
    }
    close(in);
    fsync(out);
    close(out);
    return total;
}

static long blockdev_size(const char *dev)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    long sz = lseek(fd, 0, SEEK_END);
    close(fd);
    return sz;
}

/* 重扫分区表:试 BLKRRPART,失败再试 BLKPG_ADD_PARTITION 逐个注册。
 * 注册后 /dev/<disk>N 才可用,mount/mkfs 才能挂到分区而非整盘。
 * 本内核 BLKRRPART 可能 EFAULT、BLKPG 可能成功,两条路都试,
 * 返回 1 表示至少一个分区注册成功。 */
static int rescan_partitions(const char *disk, long p1_off, long p1_size,
                             long p2_off, long p2_size)
{
    int fd = open(disk, O_RDWR);
    if (fd < 0)
        return 0;
    int ok = 0;
    if (ioctl(fd, BLKRRPART) == 0) {
        printf("  BLKRRPART ok\n");
        ok = 1;
    }
    /* BLKPG 手动注册分区 1、2(内核 UAPI blkpg_partition 内联定义,
     * start/length 以扇区为单位,pno 为分区号) */
    struct blkpg_partition p1 = { p1_off / MBR_SECTOR, p1_size / MBR_SECTOR,
                                  1, "", "" };
    struct blkpg_ioctl_arg arg1 = { BLKPG_ADD_PARTITION, 0,
                                    sizeof p1, &p1 };
    if (ioctl(fd, BLKPG, &arg1) == 0) {
        printf("  BLKPG add p1 ok\n");
        ok = 1;
    } else {
        printf("  BLKPG add p1 failed (%s)\n", strerror(errno));
    }
    struct blkpg_partition p2 = { p2_off / MBR_SECTOR, p2_size / MBR_SECTOR,
                                  2, "", "" };
    struct blkpg_ioctl_arg arg2 = { BLKPG_ADD_PARTITION, 0,
                                    sizeof p2, &arg2 };
    if (ioctl(fd, BLKPG, &arg2) == 0) {
        printf("  BLKPG add p2 ok\n");
        ok = 1;
    } else {
        printf("  BLKPG add p2 failed (%s)\n", strerror(errno));
    }
    close(fd);
    return ok;
}

static int write_mbr(const char *disk, unsigned int end_lba)
{
    int fd = open(disk, O_WRONLY);
    if (fd < 0)
        return -1;
    unsigned char mbr[MBR_SECTOR];
    memset(mbr, 0, sizeof mbr);
    /* MBR 代码: 用 syslinux 自带的 mbr.bin(440 字节, 经 mbrbin.h 内嵌)。
     * 分区表 @0x1BE 与 55AA 魔数在下方填充。
     *
     * ★ 为什么不自写(三个实测缺陷, 每个都让磁盘起不来):
     *   1. INT13 **没有 AH=0x50 读扇区**功能 —— 扩展读是 AH=0x42, 且
     *      扇区数/缓冲/LBA 全由 16 字节 DAP(DS:SI)描述; 旧代码 mov
     *      ah,0x42 却按寄存器传参, 没有 DAP, 必然读失败。
     *   2. 旧代码把 BIOS 传入的启动盘号(DL)清零(`xor dx,edx; mov dl,0`),
     *      INT13 会去读 0 号盘(软驱), VBR 也收到错的 DL。
     *   3. 最隐蔽的一条: 把 VBR 读进 0x7C00 会覆盖**正在执行的 MBR 本体**
     *      (MBR 就在 0x7C00)。INT13 返回后继续执行的是 VBR 的字节 → 跑飞,
     *      现象 = SeaBIOS "Booting from Hard Disk..." 之后无任何输出。
     *      syslinux mbr.bin 的做法: 先把自身 rep movsw 搬运到 0x0600,
     *      再从安全位置读 VBR 到 0x7C00 并远跳 —— 这才是正解。
     *
     * 该码从活动分区读 VBR, 依赖分区表里的 CHS/LBA 字段(见下方 WRITE_CHS
     * / WRITE_LBA), 且把 DL 原样传给 VBR。 */
    memcpy(mbr, syslinux_mbr_code, sizeof syslinux_mbr_code);

    /* 分区表 @ 0x1BE:2 个主分区
     * 分区 1:boot,BOOT_PART_START_LBA 起,BOOT_PART_SIZE_SECTORS 扇区
     * 分区 2:root,紧接分区 1,到磁盘末尾 */
    unsigned int boot_start = BOOT_PART_START_LBA;
    unsigned int boot_end   = boot_start + BOOT_PART_SIZE_SECTORS - 1;
    unsigned int root_start = boot_end + 1;
    unsigned int root_end   = end_lba;
    if (root_start > root_end)
        root_end = root_start + 2048 - 1;  /* root 至少给 1 MiB */
    if (root_end >= end_lba)
        root_end = end_lba;

    /* MBR 分区项 16 字节标准布局(必须严格遵守,否则 BIOS 读错):
     *   偏移  长度  字段
     *   0     1    boot flag(0x80 活动 / 0x00 非活动)
     *   1     3    CHS start(head,track_lo,track_hi)
     *   4     1    partition type(0x06 FAT16 / 0xEF ESP / 0x83 Linux)
     *   5     3    CHS end(head,track_lo,track_hi)
     *   8     4    LBA start(little-endian)
     *   12    4    sector count(little-endian)
     * 现代 BIOS 用 LBA 字段,但 SeaBIOS 1.17 对 LBA 2048 的活动分区
     * 仍会读 CHS start(0x1BE+1)来确认可引导;head 必须 < 盘磁头数,
     * track 按盘 spt 折算。用真实几何(255 磁头/63 扇道)算 CHS,
     * 末 LBA(>盘容量)时 head 顶 254、track 顶 0x7FFF。 */
    unsigned int chs_heads = 255, chs_spt = 63;
    unsigned long chs_spm = (unsigned long)chs_heads * chs_spt;

    /* 写 4 字节 LBA(little-endian)到 4 字节指针处 */
    #define WRITE_LBA(p, v) do { \
        (p)[0] = (unsigned char)((v) & 0xFF); \
        (p)[1] = (unsigned char)(((v) >> 8) & 0xFF); \
        (p)[2] = (unsigned char)(((v) >> 16) & 0xFF); \
        (p)[3] = (unsigned char)(((v) >> 24) & 0xFF); \
    } while (0)

    /* LBA→CHS 真几何折算(head<255, track 按 63 扇道):
     * head = lba / (255*63); track = (lba % (255*63)) / 63。
     * LBA 超容量(如分区 2 末尾=整盘尾)时 head 顶 254、track 顶 0x7FFF,
     * 与宿主 sfdisk/SeaBIOS 参照一致,固件读操作仍以 LBA 字段为准。 */
    #define WRITE_CHS(p, v) do { \
        unsigned long _l = (unsigned long)(v); \
        (p)[0] = (unsigned char)(_l / chs_spm); \
        unsigned long _t = (_l % chs_spm) / chs_spt; \
        if (_t > 0x7FFF) _t = 0x7FFF; \
        (p)[1] = (unsigned char)(_t & 0xFF); \
        (p)[2] = (unsigned char)((_t >> 8) & 0xFF); \
    } while (0)

    unsigned char *pt1 = mbr + 0x1BE;
    memset(pt1, 0, 16);
    pt1[0] = 0x80;                                   /* 活动(boot 标志) */
    WRITE_CHS(pt1 + 1, boot_start);                  /* CHS start */
    pt1[4] = 0x06;                                   /* FAT16 */
    WRITE_CHS(pt1 + 5, boot_end);                     /* CHS end */
    WRITE_LBA(pt1 + 8, boot_start);                  /* LBA start */
    WRITE_LBA(pt1 + 12, boot_end - boot_start + 1);   /* sector count */

    unsigned char *pt2 = mbr + 0x1CE;
    memset(pt2, 0, 16);
    pt2[0] = 0x00;                                   /* 非活动 */
    WRITE_CHS(pt2 + 1, root_start);
    pt2[4] = 0x83;                                   /* Linux 根 */
    WRITE_CHS(pt2 + 5, root_end);
    WRITE_LBA(pt2 + 8, root_start);
    WRITE_LBA(pt2 + 12, root_end - root_start + 1);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (pwrite(fd, mbr, MBR_SECTOR, 0) != MBR_SECTOR) {
        perror("write MBR");
        close(fd);
        return -1;
    }
    fsync(fd);
    close(fd);
    return 0;
}

/* 读 /sys/block/<disk>/dev,返回主:次设备号 */
static int read_dev(const char *disk, int *maj, int *min)
{
    char devfile[256];
    snprintf(devfile, sizeof devfile, "/sys/block/%s/dev", disk);
    FILE *df = fopen(devfile, "r");
    if (!df)
        return -1;
    if (fscanf(df, "%d:%d", maj, min) != 2) {
        fclose(df);
        return -1;
    }
    fclose(df);
    return 0;
}

/* 读 /sys/class/block/<disk>/dev 拿主设备号 */
static int read_major(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "/sys/class/block/%s/dev", name);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char buf[64];
    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    int maj = 0;
    if (sscanf(buf, "%d", &maj) != 1)
        return -1;
    return maj;
}

/* 设备名是否可安装(排除只读/CD-ROM/可移除) */
static int installable_disk(const char *name)
{
    int maj = read_major(name);
    if (maj < 0)
        return 0;
    if (maj == 11)          /* SCSI CD-ROM */
        return 0;
    if (maj == 2)           /* 软驱 */
        return 0;
    char rof[160];
    snprintf(rof, sizeof rof, "/sys/block/%s/ro", name);
    FILE *f = fopen(rof, "r");
    if (f) {
        char c;
        if (fread(&c, 1, 1, f) == 1 && c == '1') {
            fclose(f);
            return 0;
        }
        fclose(f);
    }
    char szf[160];
    snprintf(szf, sizeof szf, "/sys/block/%s/size", name);
    f = fopen(szf, "r");
    if (f) {
        long size = 0;
        if (fscanf(f, "%ld", &size) == 1 && size <= 0) {
            fclose(f);
            return 0;
        }
        fclose(f);
    }
    return 1;
}

    /* 写 ext2 到整盘偏移(分区 2, 无需 /dev/<disk>2 节点)。
 * 本内核 BLKPG 分区注册不可靠(BLKRRPART EFAULT + BLKPG 无效),
 * 即使 mknod 出 /dev/<disk>2 也指向未注册的 gendisk, mount 会失败。
 * 因此直接 open 整盘, 用 pwrite(绝对偏移) 在 LBA root_start_lba 处
 * 写完整 ext2(4K 块布局, 与 mkfs.c / 内核 fs/ext4 校验一致)。
 * 超级块相对分区起点字节偏移 1024(4K 块), group descriptors 在
 * 分区起点 + 4096。宿主 dumpe2fs/e2fsck 可校验。 */
static int write_ext2_whole_disk(const char *disk,
                                 unsigned int root_start_lba,
                                 unsigned int root_size_sectors)
{
    long total_bytes = (long)root_size_sectors * MBR_SECTOR;
    printf("[3/6] mkfs ext2 on partition 2 (%lu MiB, at LBA %u)...\n",
           total_bytes / (1024 * 1024), root_start_lba);

    int fd = open(disk, O_WRONLY);
    if (fd < 0) {
        perror("install: open disk for ext2");
        return 1;
    }

    off_t part_off = (off_t)root_start_lba * MBR_SECTOR;

    /* 核心参数(与 mkfs.c 完全一致) */
    unsigned long block_count = total_bytes / 4096;
    if (block_count < 200) {
        fprintf(stderr, "install: partition 2 too small for ext2 (%lu blocks)\n",
                block_count);
        close(fd);
        return 1;
    }

    unsigned long bpg = 32768;
    unsigned long groups = (block_count + bpg - 1) / bpg;
    if (groups < 1) groups = 1;
    unsigned long ipg = 8192;
    unsigned long inodes_count = groups * ipg;
    unsigned long itable_blocks = (ipg * 128 + 4095) / 4096;
    unsigned long gd_blocks = (groups * 32 + 4095) / 4096;
    /* 与 host mkfs.ext2 实测布局一致(4K 块, 与 gd_blocks 无关):
     *   0-based 块 0 = 超级块, 块 1 = GDT(1-based 值: 组 0 = 2/3/4),
     *   块 2 = block_bitmap, 块 3 = inode_bitmap,
     *   块 4..(4+itable-1) = inode 表, 块 4+itable 起 = 数据区。
     * meta(0-based 元数据块数) = 4 + itable_blocks。 */
    unsigned long meta = 4 + itable_blocks;
    unsigned long data0 = meta;         /* 0-based: 组 0 首个数据块 */

    printf("  whole-disk pwrite: %lu blocks, %lu groups, %lu inodes\n",
           block_count, groups, inodes_count);

    /* 1. 超级块(part_off + 1024) */
    unsigned char sb[4096];
    memset(sb, 0, sizeof sb);
    struct {
        unsigned int  s_inodes_count;      /* 0   */
        unsigned int  s_blocks_count;      /* 4   */
        unsigned int  s_r_blocks_count;    /* 8   */
        unsigned int  s_free_blocks_count; /* 12  */
        unsigned int  s_free_inodes_count; /* 16  */
        unsigned int  s_first_data_block;  /* 20  */
        unsigned int  s_log_block_size;    /* 24  */
        unsigned int  s_log_frag_size;     /* 28  */
        unsigned int  s_blocks_per_group;  /* 32  */
        unsigned int  s_clusters_per_group;/* 36  */
        unsigned int  s_inodes_per_group;  /* 40  */
        unsigned int  s_mtime;            /* 44  */
        unsigned int  s_wtime;            /* 48  */
        unsigned short s_mnt_count;       /* 52  */
        unsigned short s_max_mnt_count;   /* 54  */
        unsigned short s_magic;           /* 56  <- 0xEF53 */
        unsigned short s_state;           /* 58  */
        unsigned short s_errors;          /* 60  */
        unsigned short s_minor_rev_level; /* 62  */
        unsigned int   s_lastcheck;       /* 64  */
        unsigned int   s_checkinterval;   /* 68  */
        unsigned int   s_creator_os;      /* 72  */
        unsigned int   s_rev_level;       /* 76  */
        unsigned short s_def_resuid;      /* 80  */
        unsigned short s_def_resgid;      /* 82  */
        unsigned int   s_first_ino;       /* 84  */
        unsigned short s_inode_size;      /* 88  */
        unsigned short s_block_group_nr;  /* 90  */
        unsigned int   s_feature_compat;  /* 92  */
        unsigned int   s_feature_incompat;/* 96  */
        unsigned int   s_feature_ro_compat;/* 100 */
        unsigned char  s_uuid[16];        /* 104 */
        unsigned char  s_volume_name[16]; /* 120 */
    } __attribute__((packed)) *sp = (void *)sb;
    sp->s_inodes_count = inodes_count;
    sp->s_blocks_count = block_count;
    sp->s_free_inodes_count = inodes_count - 11;
    sp->s_first_data_block = 0;
    sp->s_log_block_size = 2;
    sp->s_log_frag_size = 2;
    sp->s_blocks_per_group = bpg;
    sp->s_clusters_per_group = bpg;
    sp->s_inodes_per_group = ipg;
    sp->s_mnt_count = 0;
    sp->s_max_mnt_count = 0xffff;
    sp->s_magic = 0xEF53;
    sp->s_state = 1;
    sp->s_errors = 1;
    sp->s_creator_os = 0;
    sp->s_rev_level = 0;
    sp->s_first_ino = 11;
    sp->s_inode_size = 128;
    sp->s_uuid[0] = 'P'; sp->s_uuid[1] = 'R';
    sp->s_volume_name[0] = 'P'; sp->s_volume_name[1] = 'a';
    sp->s_volume_name[2] = 'r'; sp->s_volume_name[3] = 'l';
    sp->s_volume_name[4] = 'z';

    unsigned long free_blocks_total = 0;
    for (unsigned long g = 0; g < groups; g++) {
        unsigned long gstart = g * bpg;
        unsigned long gblocks = (g == groups - 1)
            ? (block_count - gstart) : bpg;
        unsigned long m2 = meta;
        if (m2 > gblocks) m2 = gblocks;
        unsigned long used = m2 + (g == 0 ? 2 : 0);
        free_blocks_total += gblocks - used;
    }
    sp->s_free_blocks_count = free_blocks_total;

    if (pwrite(fd, sb, 4096, part_off + 1024) < 0) {
        perror("install: write ext2 superblock");
        close(fd);
        return 1;
    }

    /* 2. group descriptors(part_off + 4096) */
    unsigned char *gdt = malloc(gd_blocks * 4096);
    if (!gdt) { close(fd); return 1; }
    memset(gdt, 0, gd_blocks * 4096);
    for (unsigned long g = 0; g < groups; g++) {
        unsigned long gstart = g * bpg;
        struct {
            unsigned int   bg_block_bitmap;       /* 0  */
            unsigned int   bg_inode_bitmap;       /* 4  */
            unsigned int   bg_inode_table;        /* 8  */
            unsigned short bg_free_blocks_count;  /* 12 */
            unsigned short bg_free_inodes_count;  /* 14 */
            unsigned short bg_used_dirs_count;    /* 16 */
            unsigned short bg_flags;              /* 18 */
            unsigned int   bg_exclude_bitmap;     /* 20 */
            unsigned char  bg_reserved[8];        /* 24 */
        } __attribute__((packed)) *gd = (void *)(gdt + g * 32);
        /* GDT bg_*_block 是 1-based(组内 0 = 无效指针)。物理布局(0-based):
         * 块 0=超级块, 块 1=GDT, 块 2=block_bitmap, 块 3=inode_bitmap,
         * 块 4..(4+itable-1)=inode 表。故 1-based(组首 0-based = gstart):
         * block_bitmap = gstart + 2, inode_bitmap = gstart + 3,
         * inode_table  = gstart + 4(跨组时由 1-based 组内位置自然推得)。 */
        gd->bg_block_bitmap = gstart + 2;
        gd->bg_inode_bitmap = gstart + 3;
        gd->bg_inode_table  = gstart + 4;
        gd->bg_free_blocks_count =
            (g == groups - 1)
                ? (block_count - gstart - meta - (g == 0 ? 2 : 0))
                : (bpg - meta - (g == 0 ? 2 : 0));
        gd->bg_free_inodes_count = (g == 0) ? ipg - 11 : ipg;
        gd->bg_used_dirs_count = (g == 0) ? 2 : 0;
    }
    if (pwrite(fd, gdt, gd_blocks * 4096, part_off + 4096) < 0) {
        perror("install: write ext2 GDT");
        free(gdt);
        close(fd);
        return 1;
    }
    free(gdt);

    /* 3. inode 表(每组组内第 4 块起) */
    unsigned char *itab = malloc(itable_blocks * 4096);
    if (!itab) { close(fd); return 1; }
    for (unsigned long g = 0; g < groups; g++) {
        memset(itab, 0, itable_blocks * 4096);
        if (g == 0) {
            /* inode 2 = 根目录(目录块 0-based data0+1), inode 11 = lost+found
             * (0-based 块 data0)。
             * 关键点: ext2 i_block[0..11] 直接块指针是 0-based 块号
             * (组内 0 = 空指针), 与 mkfs.c / host mkfs.ext2 实测一致。
             * 故根目录物理位置 0-based data0+1, i_block[0] 存 data0+1;
             * lost+found 物理位置 0-based data0, i_block[0] 存 data0。
             *
             * ★ 字段偏移按 ext2 规范(mode0 uid2 size4 atime8 ctime12
             * mtime16 dtime20 gid24 **links26 blocks28** flags32
             * osd136 block40):
             *   旧代码把 i_links_count 写到 22、i_blocks_lo 写到 24 ——
             *   22 落在 i_dtime 的后半, 24 落在 i_gid。后果: lost+found
             *   的 links=0 且 i_dtime 非 0, 内核/e2fsck 视其为"已删除但仍
             *   被引用"的 inode(deleted/unused inode 11), 连带上报
             *   block/inode 位图差异、目录计数错、根链接数错, guest 挂
             *   /dev/vda2 失败(install 装完却在 initramfs 里跑)。
             *   inode 2 的同一处错被 cpfs 后续重写修正, 所以只有 11 暴露。 */
            unsigned char *i2 = itab + 128;
            *(unsigned short *)(i2 + 0)  = 0x41ED;   /* i_mode  */
            *(unsigned int   *)(i2 + 4)  = 4096;     /* i_size  */
            *(unsigned short *)(i2 + 26) = 3;        /* i_links_count(26) */
            *(unsigned int   *)(i2 + 28) = 8;        /* i_blocks_lo(28)   */
            *(unsigned int   *)(i2 + 40) = (unsigned int)(data0 + 1); /* 0-based */
            unsigned char *i11 = itab + 10 * 128;
            *(unsigned short *)(i11 + 0)  = 0x41ED;  /* i_mode  */
            *(unsigned int   *)(i11 + 4)  = 4096;    /* i_size  */
            *(unsigned short *)(i11 + 26) = 2;       /* i_links_count(26) */
            *(unsigned int   *)(i11 + 28) = 8;       /* i_blocks_lo(28)   */
            *(unsigned int   *)(i11 + 40) = (unsigned int)(data0);    /* 0-based */
        }
        off_t tab_off = part_off + (off_t)((g * bpg) + 3 + gd_blocks) * 4096;
        if (pwrite(fd, itab, itable_blocks * 4096, tab_off) < 0) {
            perror("install: write ext2 inode table");
            free(itab);
            close(fd);
            return 1;
        }
    }
    free(itab);

    /* 4. 位图 */
    unsigned char *bmp = malloc(2 * 4096);
    if (!bmp) { close(fd); return 1; }
    for (unsigned long g = 0; g < groups; g++) {
        unsigned long gstart = g * bpg;
        unsigned long gblocks = (g == groups - 1)
            ? (block_count - gstart) : bpg;
        unsigned long m2 = meta;
        if (m2 > gblocks) m2 = gblocks;
        memset(bmp, 0, 2 * 4096);
        for (unsigned long b = 0; b < m2; b++)
            bmp[b >> 3] |= (1u << (b & 7));
        if (g == 0) {
            bmp[m2 >> 3] |= (1u << (m2 & 7));
            bmp[(m2+1) >> 3] |= (1u << ((m2+1) & 7));
        }
        for (unsigned long b = gblocks; b < 4096 * 8; b++)
            bmp[b >> 3] |= (1u << (b & 7));
        if (pwrite(fd, bmp, 4096,
                    part_off + (off_t)(gstart + 1 + gd_blocks) * 4096) < 0) {
            perror("install: write ext2 block bitmap");
            free(bmp); close(fd); return 1;
        }
        memset(bmp, 0, 2 * 4096);
        if (g == 0)
            for (int b = 0; b < 11; b++)
                bmp[b >> 3] |= (1u << (b & 7));
        for (unsigned long b = ipg; b < 4096 * 8; b++)
            bmp[b >> 3] |= (1u << (b & 7));
        if (pwrite(fd, bmp, 4096,
                    part_off + (off_t)(gstart + 2 + gd_blocks) * 4096) < 0) {
            perror("install: write ext2 inode bitmap");
            free(bmp); close(fd); return 1;
        }
    }
    free(bmp);

    /* 5. 根目录与 lost+found(组 0 数据块) */
    unsigned char *dir = malloc(4096);
    if (!dir) { close(fd); return 1; }
    memset(dir, 0, 4096);
    struct { unsigned int inode; unsigned short rec_len;
            unsigned char name_len; unsigned char file_type;
            char name[8]; } __attribute__((packed)) *de;
    de = (void *)dir;
    de->inode = 2; de->rec_len = 12; de->name_len = 1;
    de->file_type = 0; de->name[0] = '.';
    de = (void *)(dir + 12);
    de->inode = 2; de->rec_len = 12; de->name_len = 2;
    de->file_type = 0; de->name[0] = '.'; de->name[1] = '.';
    de = (void *)(dir + 24);
    de->inode = 11; de->rec_len = 4096 - 24; de->name_len = 10;
    de->file_type = 0;
    memcpy(de->name, "lost+found", 10);
    pwrite(fd, dir, 4096, part_off + (off_t)(data0 + 1) * 4096);
    memset(dir, 0, 4096);
    de = (void *)dir;
    de->inode = 11; de->rec_len = 12; de->name_len = 1;
    de->file_type = 0; de->name[0] = '.';
    de = (void *)(dir + 12);
    de->inode = 2; de->rec_len = 4096 - 12; de->name_len = 2;
    de->file_type = 0; de->name[0] = '.'; de->name[1] = '.';
    pwrite(fd, dir, 4096, part_off + (off_t)data0 * 4096);
    free(dir);

    fsync(fd);
    close(fd);
    printf("  ext2 written to partition 2 (%lu blocks, magic 0xEF53)\n",
           block_count);
    return 0;
}


/* 目标盘的分区(或盘整块)当前已挂载 → 不能往它身上装。
 * "在系统内敲 install" 最大的坑: cpfs 读的是**当前根**, 写是整盘
 * pwrite 到目标分区; 若目标盘正挂着当根(或 /mnt), 等于边跑边把脚下的
 * 盘重写, 装出来的东西半新半旧且随时 ENOENT。这里直接拒绝。 */
static int target_mounted(const char *diskpath, char *dev_out, size_t n)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;
    char line[1024];
    size_t dlen = strlen(diskpath);
    int hit = 0;
    while (fgets(line, sizeof line, f)) {
        char dev[256];
        if (sscanf(line, "%255s", dev) != 1)
            continue;
        if (strncmp(dev, diskpath, dlen) != 0)
            continue;
        /* 命中 /dev/vda 本身, 或 /dev/vda1、/dev/vda2 这类分区 */
        char c = dev[dlen];
        if (c == '\0' || (c >= '0' && c <= '9')) {
            snprintf(dev_out, n, "%s", dev);
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}

int main(int argc, char *argv[])
{
    const char *disk;
    char partname[256];

    if (argc >= 2)
        disk = argv[1];
    else {
        /* 自动探测:遍历 /sys/block,找第一个可安装磁盘。
         * 排除:loop/ram/fd/sr*(光驱)/只读/空设备。优先 vd*,再 sd*,再 hd*。 */
        DIR *d = opendir("/sys/block");
        if (!d) {
            fprintf(stderr, "install: cannot open /sys/block\n");
            return 1;
        }
        struct dirent *e;
        disk = NULL;
        int best_rank = 100;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            /* 按前缀排优先级 */
            int rank = 100;
            if (strncmp(e->d_name, "vd", 2) == 0)
                rank = 0;
            else if (strncmp(e->d_name, "sd", 2) == 0)
                rank = 1;
            else if (strncmp(e->d_name, "hd", 2) == 0)
                rank = 2;
            else if (strncmp(e->d_name, "nvme", 4) == 0)
                rank = 3;
            if (strncmp(e->d_name, "loop", 4) == 0 ||
                strncmp(e->d_name, "ram", 3) == 0 ||
                strncmp(e->d_name, "fd", 2) == 0 ||
                strncmp(e->d_name, "sr", 2) == 0 ||
                strncmp(e->d_name, "zram", 4) == 0)
                continue;
            if (!installable_disk(e->d_name))
                continue;
            if (rank >= best_rank)
                continue;
            /* 确保 /dev/<name> 节点存在 */
            char devnode[64];
            snprintf(devnode, sizeof devnode, "/dev/%s", e->d_name);
            struct stat st;
            if (stat(devnode, &st) != 0) {
                int maj = read_major(e->d_name);
                char devf[256];
                snprintf(devf, sizeof devf,
                         "/sys/block/%s/dev", e->d_name);
                FILE *df = fopen(devf, "r");
                int min = 0;
                if (df) {
                    if (fscanf(df, "%d:%d", &maj, &min) != 2)
                        maj = -1;
                    fclose(df);
                }
                if (maj >= 0)
                    mknod(devnode, S_IFBLK | 0660,
                          ((maj & 0xfff) << 8) | (min & 0xff));
                if (stat(devnode, &st) != 0)
                    continue;
            }
            disk = e->d_name;
            best_rank = rank;
        }
        closedir(d);
        if (!disk) {
            fprintf(stderr, "install: no installable disk found "
                            "(sr*/loop/ram/readonly skipped)\n");
            return 1;
        }
        printf("install: auto-detected disk %s\n", disk);
    }
    /* disk 形如 "vda" 或 "/dev/vda",统一成 /dev/vda */
    char diskpath[256];
    if (strncmp(disk, "/dev/", 5) == 0)
        snprintf(diskpath, sizeof diskpath, "%s", disk);
    else
        snprintf(diskpath, sizeof diskpath, "/dev/%s", disk);

    snprintf(partname, sizeof partname, "%s1", diskpath);
    char partname2[256];
    snprintf(partname2, sizeof partname2, "%s2", diskpath);

    printf("=== Parlz installer v0.3 (syslinux 双分区) ===\n");
    printf("Disk: %s\n", diskpath);
    printf("Partition 1 (boot, FAT16+syslinux): %s\n", partname);
    printf("Partition 2 (root): %s\n", partname2);

    {
        char mdev[256] = "";
        if (target_mounted(diskpath, mdev, sizeof mdev)) {
            fprintf(stderr, "install: 拒绝安装 —— %s 的 %s 当前已挂载,"
                            " 系统正跑在这块盘上\n", diskpath, mdev);
            fprintf(stderr, "install: 重装请从安装介质启动"
                            "(scripts/boot-install.sh 或 ISO), 或先 umount\n");
            return 1;
        }
    }

    int pmaj = 0, pmin = 0;
    if (read_dev(diskpath + 5, &pmaj, &pmin) != 0) {
        fprintf(stderr, "install: cannot read /sys/block/%s/dev\n",
                diskpath + 5);
        return 1;
    }
    int part1_min = pmin + 1;   /* 分区 1 */
    int part2_min = pmin + 2;   /* 分区 2 */
    printf("  disk dev: %d:%d, partition 1: %d:%d, partition 2: %d:%d\n",
           pmaj, pmin, pmaj, part1_min, pmaj, part2_min);

    long disk_bytes = blockdev_size(diskpath);
    if (disk_bytes < 0) {
        fprintf(stderr, "install: cannot read size of %s\n", diskpath);
        return 1;
    }
    unsigned int total_sectors = disk_bytes / MBR_SECTOR;
    unsigned int end_lba = total_sectors - 1;
    unsigned int boot_start = BOOT_PART_START_LBA;
    unsigned int boot_end   = boot_start + BOOT_PART_SIZE_SECTORS - 1;
    unsigned int root_start = boot_end + 1;
    unsigned int root_end   = end_lba;
    if (end_lba <= boot_end) {
        fprintf(stderr, "install: disk too small for 2 partitions\n");
        return 1;
    }
    printf("  disk: %ld bytes (%u sectors)\n", disk_bytes, total_sectors);
    printf("  partition 1 (boot): LBA %u .. %u (%u MiB)\n",
           boot_start, boot_end,
           (boot_end - boot_start + 1) * MBR_SECTOR / (1024 * 1024));
    printf("  partition 2 (root): LBA %u .. %u (%u MiB)\n",
           root_start, root_end,
           (root_end - root_start + 1) * MBR_SECTOR / (1024 * 1024));

    /* [0/6] 引导镜像预检: 缺镜像就在**动盘之前**退出。
     * 旧行为是写完 MBR 才在 [2/6] 报 "找不到引导镜像", 盘上留下一个
     * 有分区表但没引导分区/根分区的半成品(重跑可修, 但容易让人以为
     * 装坏了)。镜像 fd 一直留到 [2/6] 用。 */
    long bootimg_bytes = 0;
    int bootimg_fd = -1;
    printf("[0/6] 检查引导镜像源...\n");
    bootimg_fd = open_boot_image(diskpath, &bootimg_bytes);
    if (bootimg_fd < 0)
        return 1;
    printf("  引导镜像 %ld 字节, 分区 1 容量 %ld 字节: %s\n",
           bootimg_bytes, (long)BOOT_PART_SIZE_SECTORS * MBR_SECTOR,
           bootimg_bytes <= (long)BOOT_PART_SIZE_SECTORS * MBR_SECTOR
               ? "OK" : "超出!");

    /* [1/6] MBR(syslinux mbr.bin + 双分区表 + 魔数) */
    printf("[1/6] Writing MBR (syslinux mbr.bin + 2 partitions)...\n");
    if (write_mbr(diskpath, end_lba) < 0) {
        close(bootimg_fd);
        return 1;
    }
    printf("  MBR written\n");

    /* 分区注册:本内核 BLKRRPART 返回 EFAULT、BLKPG 无效,
     * 走整盘 pwrite 兜底。若未来内核支持注册,整盘写也兼容。 */
    rescan_partitions(diskpath,
                      (long)boot_start * MBR_SECTOR,
                      (long)(boot_end - boot_start + 1) * MBR_SECTOR,
                      (long)root_start * MBR_SECTOR,
                      (long)(root_end - root_start + 1) * MBR_SECTOR);
    {
        /* 等分区节点出现(最多 ~3s):本内核不注册,快速失败后继续
         * 走整盘写路径。 */
        char sysp1[128];
        snprintf(sysp1, sizeof sysp1, "/sys/block/%s/%s1",
                 diskpath + 5, diskpath + 5);
        for (int w = 0; w < 60; w++) {
            if (access(sysp1, F_OK) == 0) {
                printf("  partition %s1 registered after %d ms\n",
                       diskpath + 5, w * 50);
                break;
            }
            usleep(50 * 1000);
        }
        if (access(sysp1, F_OK) != 0)
            printf("  partition not registered,用整盘写兜底(正常)\n");
    }

    /* 建分区 1、分区 2 的 dev 节点。devtmpfs 异步创建设备节点,
     * BLKRRPART 后先轮询等待(最多 ~5s);仍未出现则按盘主设备号 +1/+2
     * 手动 mknod 兜底(BUG-6:节点未及时出现导致后续挂载/写入跳过)。 */
    struct stat pstat;
    for (int w = 0; w < 50; w++) {
        if (stat(partname, &pstat) == 0 &&
            stat(partname2, &pstat) == 0)
            break;
        usleep(100 * 1000);
    }
    if (stat(partname, &pstat) != 0) {
        if (mknod(partname, S_IFBLK | 0660,
                  ((pmaj & 0xfff) << 8) | (part1_min & 0xff)) == 0)
            printf("  created %s (blk %d:%d)\n",
                   partname, pmaj, part1_min);
        else
            printf("  warning: mknod %s failed (%s)\n",
                   partname, strerror(errno));
    }
    if (stat(partname2, &pstat) != 0) {
        if (mknod(partname2, S_IFBLK | 0660,
                  ((pmaj & 0xfff) << 8) | (part2_min & 0xff)) == 0)
            printf("  created %s (blk %d:%d)\n",
                   partname2, pmaj, part2_min);
        else
            printf("  warning: mknod %s failed (%s)\n",
                   partname2, strerror(errno));
    }

    /* [2/6] 分区 1:写 FAT16 引导分区(host 侧 gen-fatboot.sh 生成的
     * fat16_image,40 MiB,内含 ldlinux.sys + vmlinuz + bootx64.efi +
     * syslinux.cfg)。本内核分区不注册,/dev/vda1 不可用,直接
     * pwrite 整盘 LBA 偏移。
     *
     * ★ 关键顺序:必须在 [3/6] 之前写,且 [3/6] ext2 写入不得越界进分区 1。
     * 旧 bug:write_ext2_whole_disk 把 ext2 写到 LBA 2048(分区 1 起点),
     * 覆盖了 VBR 引导代码(EB 3C 90),导致 SeaBIOS "Booting from Hard
     * Disk.." 后静默。修复:ext2 只在分区 2(root_start..root_end)内
     * 分配块,part_off 起点改为 root_start_lba(非 2048)。 */
    printf("[2/6] Writing FAT16 boot partition (%ld MiB) to LBA %u...\n",
           (long)(BOOTFAT_IMAGE_BYTES / (1024 * 1024)), boot_start);
    if (write_boot_partition(diskpath, bootimg_fd, bootimg_bytes) < 0) {
        close(bootimg_fd);
        return 1;
    }
    close(bootimg_fd);
    printf("  FAT16 boot image: %u bytes "
           "(ldlinux.sys + vmlinuz + bootx64.efi + syslinux.cfg)\n",
           (unsigned)BOOTFAT_IMAGE_BYTES);
    sync();

    /* [3/6] 分区 2:mkfs ext2(内核 ext4 驱动可挂载)。
     * 本内核 BLKPG 分区注册不可靠(BLKRRPART EFAULT + BLKPG 无效),
     * mknod 出的 /dev/<disk>2 指向未注册的 gendisk,open 会阻塞,
     * 因此直接用整盘 pwrite:在 LBA root_start 偏移写完整 ext2
     * (布局与 mkfs.c 一致,dumpe2fs/e2fsck 可校验)。
     * ext2 只在分区 2 内(root_start..root_end),绝不越界进分区 1。
     * 格式化后再把 rootfs(当前 initramfs 所在根)拷进分区 2,
     * 磁盘自启时 root=/dev/vda2 挂载到真根并 pivot,避免内核
     * 在 vda2 里找 /init 找不到导致 "No working init found"。 */
    printf("[3/6] mkfs ext2 on partition 2 (root, %u MiB)...\n",
           (root_end - root_start + 1) * MBR_SECTOR / (1024 * 1024));
    {
        /* root_size = root_end - root_start + 1(分区 2 大小),
         * root_start = 分区 2 起点 LBA(非 2048,非 0)。
         * write_ext2_whole_disk 内部 part_off = root_start_lba * 512,
         * block_count = (root_end - root_start + 1) * 512 / 4096。 */
        unsigned int root_size_sectors = root_end - root_start + 1;
        int wrc = write_ext2_whole_disk(diskpath, root_start,
                                        root_size_sectors);
        if (wrc < 0)
            return 1;
    }

    /* [3b/6] 拷贝 rootfs 到分区 2:fork + execv /bin/cpfs,
     * 遍历当前根(=initramfs 根),按 ext2 动态块分配把文件 pwrite
     * 到分区 2 偏移。挂载点只建空目录、符号链接照拷(见 cpfs.c)。
     *
     * ★ cpfs 失败 = 安装**没完成**, 必须非零退出: 以前只打一行警告就
     * 继续往下走, 照样写 install-done 标记、照样报 "install complete"
     * —— 结果下次启动 MBR/VBR/内核全通, 挂上的分区 2 却是个空根
     * (连 /bin 都没有), 表现为 "pivot_root OK 然后立刻 (shell exited)"。
     * 报假成功比报失败危险得多。 */
    printf("[3b/6] Copying rootfs to partition 2...\n");
    int rootfs_copied = 0;
    {
        pid_t cpid = fork();
        if (cpid == 0) {
            char s_lba[32], s_tot[32];
            snprintf(s_lba, sizeof s_lba, "%u", root_start);
            snprintf(s_tot, sizeof s_tot, "%u", total_sectors);
            if (chdir("/") != 0)   /* cpfs 从 / 开始遍历(initramfs 根) */
                perror("install: chdir /");
            execl("/bin/cpfs", "cpfs", diskpath, s_lba, s_tot,
                  (char *)NULL);
            _exit(127);
        }
        int cst;
        waitpid(cpid, &cst, 0);
        if (WIFEXITED(cst) && WEXITSTATUS(cst) == 0) {
            rootfs_copied = 1;
            printf("  rootfs copied to partition 2\n");
        } else if (WIFEXITED(cst)) {
            fprintf(stderr, "  cpfs 失败(退出码 %d) —— 分区 2 里的根不完整\n",
                    WEXITSTATUS(cst));
        } else {
            fprintf(stderr, "  cpfs 被信号 %d 打断 —— 分区 2 里的根不完整\n",
                    WTERMSIG(cst));
        }
    }

    /* [4/6] 挂载验证不可靠(分区设备未注册,open 会阻塞),
     * 跳过 mount test —— 直接验证下次 boot 时 init 挂载 /dev/vda2 */
    printf("[4/6] Skip mount test (partition node not registered, "
           "verify at boot via init)\n");

    /* [5/6] 跳过独立 initramfs 拷贝(根已内嵌在 vmlinuz) */
    printf("[5/6] Skip initramfs copy (embedded in vmlinuz)\n");
    sync();

    /* 写安装完成标记:整盘 pwrite 到 LBA 1(MBR 与 boot 分区之间的
     * 空闲区;原先写在 boot_end+1 = root 分区起点,root 一旦格式化
     * 标记即被覆盖,导致每次启动都重跑 /install.d)。
     * init 启动时探测该字符串:有则跳过安装(磁盘自举),
     * 无则自动跑 /install.d(ISO 自举兜底)。 */
    {
        int fd = -1;
        if (rootfs_copied)
            fd = open(diskpath, O_WRONLY);
        const char *mark = "parlz install done";
        long moff = 1L * MBR_SECTOR;
        if (fd >= 0 && pwrite(fd, mark, strlen(mark), moff) > 0)
            printf("[5b] install-done marker written (LBA 1)\n");
        else if (rootfs_copied)
            printf("[5b] marker write skipped\n");
        else
            printf("[5b] 不写 install-done 标记(rootfs 没拷成功, 下次启动还会重试安装)\n");
        if (fd >= 0)
            close(fd);
        sync();
    }

    /* [6/6] 确认 MBR 魔数 + syslinux 引导链就位 */
    {
        int fd = open(diskpath, O_RDONLY);
        unsigned char chk[2];
        long ok = 0;
        if (fd >= 0) {
            if (pread(fd, chk, 2, 510) == 2 &&
                chk[0] == 0x55 && chk[1] == 0xAA)
                ok = 1;
            close(fd);
        }
        printf("[6/6] MBR signature %s "
               "(syslinux mbr.bin + FAT16 boot partition)\n",
               ok ? "OK" : "MISSING");
    }

    sync();
    if (!rootfs_copied) {
        fprintf(stderr, "\n=== install FAILED ===\n");
        fprintf(stderr, "install: 引导链已写好(MBR/分区 1), 但分区 2 的根不完整,"
                        " 不能从这块盘启动\n");
        fprintf(stderr, "install: 看上面 cpfs 的输出定位原因, 修好后重跑 install\n");
        return 1;
    }
    printf("\n=== install complete ===\n");
    printf("Reboot to boot Parlz from %s (boot) + %s (root)\n",
           partname, partname2);
    printf("Legacy: SeaBIOS 读 MBR(syslinux mbr.bin)→ /ldlinux.sys\n");
    printf("UEFI:   OVMF 读 /BOOTX64.EFI(syslinux.efi)\n");
    return 0;
}
