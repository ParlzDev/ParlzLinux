/* mkfs.c - 内置 ext2 文件系统创建器(无外部依赖)。
 * 用法: mkfs <device> [size-in-MiB]
 *   例如: mkfs /dev/vda1 64
 *
 * 在块设备上写完整的 ext2 文件系统,让 Linux 的 ext4 驱动能挂载它。
 * 安装器用它格式化目标分区。
 *
 * 关键布局(4K 块,与内核 fs/ext4 的校验对齐):
 *   - 超级块固定在分区字节偏移 1024(4K 块时在块 0 内偏移 1024,
 *     不是块 1 —— 这是最容易踩的坑)
 *   - s_first_data_block = 0(4K 块;1K 块才是 1)
 *   - s_magic 在超块偏移 56、s_inode_size 偏移 88、s_first_ino 偏移 84
 *   - group descriptor 的 bg_block_bitmap 在偏移 0、bg_inode_bitmap 偏移 4
 *   - s_inodes_count 必须 == groups * s_inodes_per_group(内核强校验)
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define EXT2_SUPER_MAGIC  0xEF53
#define BLOCK_SIZE        4096
#define INODE_SIZE        128
#define SUPER_OFFSET      1024          /* 超级块固定字节偏移 */
#define BLKGETSIZE64      0x80081272
#define BLKGETSIZE        0x80041268

/* ext2 inode(128 字节,字段偏移按内核 ext4_inode) */
struct ext2_inode {
    unsigned short  i_mode;          /* 0  */
    unsigned short  i_uid;           /* 2  */
    unsigned int    i_size_lo;       /* 4  */
    unsigned int    i_atime;         /* 8  */
    unsigned int    i_ctime;         /* 12 */
    unsigned int    i_mtime;         /* 16 */
    unsigned int    i_dtime;         /* 20 */
    unsigned short  i_gid;           /* 24 */
    unsigned short  i_links_count;   /* 26 */
    unsigned int    i_blocks_lo;     /* 28 */
    unsigned int    i_flags;         /* 32 */
    unsigned int    i_osd1;          /* 36 */
    unsigned int    i_block[15];     /* 40 */
    unsigned int    i_generation;    /* 100 */
    unsigned int    i_file_acl_lo;   /* 104 */
    unsigned int    i_size_high;     /* 108 */
    unsigned int    i_obso_faddr;    /* 112 */
    unsigned char   i_osd2[12];      /* 116 */
} __attribute__((packed));

/* ext2 group descriptor — 按内核 ext4_group_desc 偏移:
 * bg_block_bitmap(0) / bg_inode_bitmap(4) / bg_inode_table(8) /
 * free_blocks(12) / free_inodes(14) / used_dirs(16) / flags(18) */
struct ext2_group_desc {
    unsigned int   bg_block_bitmap;       /* 0  */
    unsigned int   bg_inode_bitmap;       /* 4  */
    unsigned int   bg_inode_table;        /* 8  */
    unsigned short bg_free_blocks_count;  /* 12 */
    unsigned short bg_free_inodes_count;  /* 14 */
    unsigned short bg_used_dirs_count;    /* 16 */
    unsigned short bg_flags;              /* 18 */
    unsigned int   bg_exclude_bitmap;     /* 20 */
    unsigned char  bg_reserved[8];        /* 24 */
} __attribute__((packed));

/* ext2 superblock — 按内核 ext4_super_block 偏移 */
struct ext2_super {
    unsigned int   s_inodes_count;      /* 0   */
    unsigned int   s_blocks_count;      /* 4   */
    unsigned int   s_r_blocks_count;    /* 8   */
    unsigned int   s_free_blocks_count; /* 12  */
    unsigned int   s_free_inodes_count; /* 16  */
    unsigned int   s_first_data_block;  /* 20  */
    unsigned int   s_log_block_size;    /* 24  */
    unsigned int   s_log_frag_size;     /* 28  */
    unsigned int   s_blocks_per_group;  /* 32  */
    unsigned int   s_clusters_per_group;/* 36  */
    unsigned int   s_inodes_per_group;  /* 40  */
    unsigned int   s_mtime;             /* 44  */
    unsigned int   s_wtime;             /* 48  */
    unsigned short s_mnt_count;        /* 52  */
    unsigned short s_max_mnt_count;    /* 54  */
    unsigned short s_magic;            /* 56  <- EXT2_SUPER_MAGIC */
    unsigned short s_state;            /* 58  */
    unsigned short s_errors;           /* 60  */
    unsigned short s_minor_rev_level;  /* 62  */
    unsigned int   s_lastcheck;        /* 64  */
    unsigned int   s_checkinterval;    /* 68  */
    unsigned int   s_creator_os;       /* 72  */
    unsigned int   s_rev_level;        /* 76  */
    unsigned short s_def_resuid;       /* 80  */
    unsigned short s_def_resgid;       /* 82  */
    unsigned int   s_first_ino;        /* 84  */
    unsigned short s_inode_size;       /* 88  */
    unsigned short s_block_group_nr;   /* 90  */
    unsigned int   s_feature_compat;   /* 92  */
    unsigned int   s_feature_incompat; /* 96  */
    unsigned int   s_feature_ro_compat;/* 100 */
    unsigned char  s_uuid[16];         /* 104 */
    unsigned char  s_volume_name[16];  /* 120 */
    unsigned char  s_last_mounted[64]; /* 136 */
} __attribute__((packed));

/* ext2 目录条目 */
struct dirent2 {
    unsigned int   inode;
    unsigned short rec_len;
    unsigned char  name_len;
    unsigned char  file_type;
    char           name[];
} __attribute__((packed));

static long device_size(int fd)
{
    unsigned long long size = 0;
    if (ioctl(fd, BLKGETSIZE64, &size) < 0)
        return -1;
    return (long)size;
}

static long partition_size(const char *dev)
{
    char disk[256];
    snprintf(disk, sizeof disk, "%s", dev);
    char *p1 = strrchr(disk, '1');
    if (!p1)
        return -1;
    *p1 = '\0';
    int fd = open(disk, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned char mbr[512];
    if (read(fd, mbr, 512) != 512) { close(fd); return -1; }
    close(fd);
    for (int i = 0; i < 4; i++) {
        unsigned char *pt = mbr + 0x1BE + 16 * i;
        if (pt[0] == 0)
            continue;
        unsigned int nsec = (unsigned int)pt[8] |
            ((unsigned int)pt[9] << 8) | ((unsigned int)pt[10] << 16) |
            ((unsigned int)pt[11] << 24);
        if (i == 0)
            return (long)nsec * 512;
    }
    return -1;
}

static long dev_total_sectors(int fd)
{
    unsigned long sec = 0;
    if (ioctl(fd, BLKGETSIZE, &sec) < 0)
        return -1;
    return (long)sec;
}

/* set_bit 位图工具 */
static void set_bit(unsigned char *map, unsigned long bit)
{
    map[bit >> 3] |= (1u << (bit & 7));
}

/* mkfs_ext2_fd: 在 fd 上写完整 ext2 文件系统(4K 块布局,
 * 内核 ext4 驱动可挂载)。size_bytes < 0 时 fd 必须是块设备(自动探测
 * 大小)。pwrite 带 LBA 偏移, 写目标不是 0(整盘偏移写分区)也可用。
 * 返回 0 成功、-1 失败。install.c 用它向整盘 pwrite 格式化分区 2,
 * 无需 /dev/<disk>2 节点(本内核分区不注册)。 */
int mkfs_ext2_fd(int fd, long size_bytes, const char *label)
{
    long total_bytes = size_bytes;
    if (total_bytes <= 0) {
        if (size_bytes < 0) {
            total_bytes = device_size(fd);
            if (total_bytes < 0)
                total_bytes = partition_size(label ? label : "dev");
            if (total_bytes < 0) {
                long sec = dev_total_sectors(fd);
                if (sec > 0)
                    total_bytes = sec * 512;
            }
        }
        if (total_bytes <= 0) {
            fprintf(stderr, "mkfs: cannot determine size\n");
            return -1;
        }
    }
    const char *dev = label ? label : "(fd)";

    unsigned long block_count = total_bytes / BLOCK_SIZE;
    if (block_count < 200) {
        fprintf(stderr, "mkfs: device too small (%lu blocks)\n", block_count);
        return -1;
    }

    /* --- 参数 --- */
    unsigned long bpg = 32768;                       /* 4K 块的最大每组块数 */
    unsigned long groups = (block_count + bpg - 1) / bpg;
    if (groups < 1) groups = 1;
    unsigned long ipg = 8192;                        /* 每组 inode 数 */
    unsigned long inodes_count = groups * ipg;
    unsigned long itable_blocks = (ipg * INODE_SIZE + BLOCK_SIZE - 1)
                                  / BLOCK_SIZE;
    unsigned long gd_blocks = (groups * 32 + BLOCK_SIZE - 1) / BLOCK_SIZE;

    /* 每组元数据占块数(0-based):
     *   块 0 = 超级块(块内偏移 1024), 块 1 = GDT,
     *   块 2 = block_bitmap, 块 3 = inode_bitmap,
     *   块 4..(3+itable) = inode 表
     * => 头 = 4 + itable_blocks(0-based 块号);
     *    组 0 首数据块 data0 = 4 + itable_blocks(0-based)
     *    i_block 存 0-based 块号(组内 0 = 空指针) */
    unsigned long group_meta = 4 + itable_blocks;
    unsigned long meta_per_group = group_meta;

    printf("mkfs: ext2 on %s: %lu blocks, %lu groups, %lu inodes, "
           "%lu itable blocks, %lu meta/group\n",
           dev, block_count, groups, inodes_count, itable_blocks,
           meta_per_group);

    /* --- 1. 超级块(字节偏移 1024) --- */
    unsigned char sb_area[BLOCK_SIZE];
    memset(sb_area, 0, sizeof sb_area);
    struct ext2_super *s = (struct ext2_super *)sb_area;
    s->s_inodes_count = inodes_count;
    s->s_blocks_count = block_count;
    s->s_r_blocks_count = 0;
    s->s_free_inodes_count = inodes_count - 11;  /* inode 1..11 已用 */
    s->s_first_data_block = 0;                   /* 4K 块 -> 0 */
    s->s_log_block_size = 2;                     /* 4K */
    s->s_log_frag_size = 2;
    s->s_blocks_per_group = bpg;
    s->s_clusters_per_group = bpg;
    s->s_inodes_per_group = ipg;
    s->s_mnt_count = 0;
    s->s_max_mnt_count = 0xffff;
    s->s_magic = EXT2_SUPER_MAGIC;
    s->s_state = 1;                              /* EXT2_VALID_FS */
    s->s_errors = 1;                             /* EXT2_ERRORS_CONTINUE */
    s->s_minor_rev_level = 0;
    s->s_creator_os = 0;                         /* EXT2_OS_LINUX */
    s->s_rev_level = 0;                          /* EXT2_GOOD_OLD_REV */
    s->s_first_ino = 11;
    s->s_inode_size = INODE_SIZE;
    /* RO_COMPAT_LARGE_FILE(0x2): 允许常规文件 i_size 超过一次
     * 二级间接块容量(>12MB @4K 块)。cpfs 写 4..8MB 大文件时,
     * 不带此标志 e2fsck/内核按"满容量"回算 i_size 会误报
     * "i_size should be 12582912"(=3072 块), 且 >4MB 文件读取
     * 可能被截断。host mkfs.ext2 亦设此位(4K 块默认)。 */
    s->s_feature_ro_compat = 0x0002;
    s->s_uuid[0] = 'P'; s->s_uuid[1] = 'R';
    s->s_volume_name[0] = 'P'; s->s_volume_name[1] = 'a';
    s->s_volume_name[2] = 'r'; s->s_volume_name[3] = 'l'; s->s_volume_name[4] = 'z';

    /* 统计已用块:每组元数据 meta 个,组 0 另有 2 个数据块(lost+found 与 /) */
    unsigned long free_blocks_total = 0;
    for (unsigned long g = 0; g < groups; g++) {
        unsigned long gstart = g * bpg;
        unsigned long gblocks = (g == groups - 1)
            ? (block_count - gstart) : bpg;
        unsigned long meta = meta_per_group;
        if (meta > gblocks) meta = gblocks;
        unsigned long used = meta + (g == 0 ? 2 : 0);
        free_blocks_total += gblocks - used;
    }
    s->s_free_blocks_count = free_blocks_total;

    /* 写超级块(4K 块:块 0 内偏移 1024) */
    if (pwrite(fd, sb_area, BLOCK_SIZE, SUPER_OFFSET) < 0) {
        perror("mkfs: write superblock");
        return -1;
    }

    /* --- 2. group descriptors(块 1 起,每组一个 32 字节条目) ---
     * 所有 bg_*_block 字段是 1-based(组内 0 = 无效)。物理布局(0-based):
     *   块 0 = 超级块, 块 1 = GDT, 块 2 = block_bitmap,
     *   块 3 = inode_bitmap, 块 4..(3+itable) = inode 表,
     *   块 (4+itable) 起 = 数据块。 */
    unsigned char *gdt = malloc(gd_blocks * BLOCK_SIZE);
    if (!gdt) return -1;
    memset(gdt, 0, gd_blocks * BLOCK_SIZE);
    for (unsigned long g = 0; g < groups; g++) {
        struct ext2_group_desc *gd =
            (struct ext2_group_desc *)(gdt + g * 32);
        unsigned long gstart = g * bpg;
        /* GDT 字段是 1-based(组内 0 = 无效指针)。物理布局(0-based):
         * 块 1=GDT, 块 2=block_bitmap, 块 3=inode_bitmap,
         * 块 4..(4+itable-1)=inode 表, 块 (4+itable) 起=数据块。
         * 故 1-based: block_bitmap=组首+2, inode_bitmap=组首+3,
         *             inode_table=组首+4。 */
        gd->bg_block_bitmap = gstart + 2;
        gd->bg_inode_bitmap = gstart + 3;
        gd->bg_inode_table  = gstart + 4;
        gd->bg_free_blocks_count =
            (g == groups - 1)
                ? (block_count - gstart - meta_per_group - (g == 0 ? 2 : 0))
                : (bpg - meta_per_group - (g == 0 ? 2 : 0));
        gd->bg_free_inodes_count = (g == 0) ? ipg - 11 : ipg;
        gd->bg_used_dirs_count = (g == 0) ? 2 : 0;
        gd->bg_flags = 0;
    }
    if (pwrite(fd, gdt, gd_blocks * BLOCK_SIZE, BLOCK_SIZE) < 0) {
        perror("mkfs: write group descriptors");
        free(gdt);
        return -1;
    }
    free(gdt);

    /* --- 3. inode 表(每组组内第 4 块起, 0-based 块 3+gd_blocks) --- */
    unsigned char *itab = malloc(itable_blocks * BLOCK_SIZE);
    if (!itab) return -1;

    for (unsigned long g = 0; g < groups; g++) {
        memset(itab, 0, itable_blocks * BLOCK_SIZE);
        unsigned long gstart = g * bpg;
        /* 组 0 的第一个数据块(0-based): 头 = 4 + itable_blocks,
         * 4K 块时 gd_blocks=1, data0 = 4 + itable_blocks (0-based)。
         * ext2 i_block 是 0-based 块号(组内 0 = 空指针), 与 host
         * mkfs.ext2 实测一致。 */
        unsigned long data0 = gstart + meta_per_group;   /* 0-based 组首数据块 */

        if (g == 0) {
            /* inode 1..10 是 ext2 保留区(bad-blocks inode 等),留空不填。
             * inode 2 = / ,inode 11 = lost+found(s_first_ino 指向的第一个
             * 可分配 inode,标准 mke2fs 也把 lost+found 放这里)。 */

            /* inode 2: / (目录块 0-based data0+1) */
            struct ext2_inode *i2 =
                (struct ext2_inode *)(itab + INODE_SIZE);
            i2->i_mode = 0x41ED;             /* drwxr-xr-x */
            i2->i_size_lo = BLOCK_SIZE;
            i2->i_links_count = 3;
            i2->i_blocks_lo = BLOCK_SIZE / 512;
            i2->i_block[0] = data0 + 1;      /* 0-based */

            /* inode 11: lost+found (目录块 0-based data0) */
            struct ext2_inode *i11 =
                (struct ext2_inode *)(itab + 10 * INODE_SIZE);
            i11->i_mode = 0x41FF;            /* drwxrwxrwx */
            i11->i_size_lo = BLOCK_SIZE;
            i11->i_links_count = 2;
            i11->i_blocks_lo = BLOCK_SIZE / 512;
            i11->i_block[0] = data0;         /* 0-based */
        }

        off_t tab_off = (off_t)(gstart + 3 + gd_blocks) * BLOCK_SIZE;
        if (pwrite(fd, itab, itable_blocks * BLOCK_SIZE, tab_off) < 0) {
            perror("mkfs: write inode table");
            free(itab);
            return -1;
        }
    }
    free(itab);

    /* --- 4. 位图(每组:block_bitmap 组内第 2 块,inode_bitmap 第 3 块) --- */
    unsigned char *bmp = malloc(2 * BLOCK_SIZE);
    if (!bmp) return -1;

    for (unsigned long g = 0; g < groups; g++) {
        unsigned long gstart = g * bpg;
        unsigned long gblocks = (g == groups - 1)
            ? (block_count - gstart) : bpg;
        unsigned long meta = meta_per_group;
        if (meta > gblocks) meta = gblocks;

        /* block bitmap:标记组内已用块。
         * 关键:超出本组实际块数的位(padding)必须置 1,否则内核报
         * "padding at end of block bitmap is not set" 并拒绝该组分配。 */
        memset(bmp, 0, 2 * BLOCK_SIZE);
        for (unsigned long b = 0; b < meta; b++)
            set_bit(bmp, b);
        if (g == 0) {
            set_bit(bmp, meta);       /* lost+found 数据块 */
            set_bit(bmp, meta + 1);   /* / 数据块 */
        }
        for (unsigned long b = gblocks; b < BLOCK_SIZE * 8; b++)
            set_bit(bmp, b);          /* padding */
        if (pwrite(fd, bmp, BLOCK_SIZE,
                   (off_t)(gstart + 1 + gd_blocks) * BLOCK_SIZE) < 0) {
            perror("mkfs: write block bitmap");
            free(bmp); return -1;
        }

        /* inode bitmap:组 0 需标记 inode 1..10(保留区)与 inode 11
         * (lost+found)为已用。inode 号从 1 起、位序从 0 起:inode N -> bit(N-1)。 */
        memset(bmp, 0, 2 * BLOCK_SIZE);
        if (g == 0) {
            for (int b = 0; b < 11; b++)
                set_bit(bmp, b);      /* inode 1..11 */
        }
        for (unsigned long b = ipg; b < BLOCK_SIZE * 8; b++)
            set_bit(bmp, b);          /* padding */
        if (pwrite(fd, bmp, BLOCK_SIZE,
                   (off_t)(gstart + 2 + gd_blocks) * BLOCK_SIZE) < 0) {
            perror("mkfs: write inode bitmap");
            free(bmp); return -1;
        }
    }
    free(bmp);

    /* --- 5. 根目录与 lost+found 内容 --- */
    unsigned char *dirblk = malloc(BLOCK_SIZE);
    if (!dirblk) return -1;
    unsigned long data0 = meta_per_group;      /* 组 0 首数据块 */

    /* / :. 、 .. 和 lost+found 三个条目 */
    memset(dirblk, 0, BLOCK_SIZE);
    {
        struct dirent2 *de = (struct dirent2 *)dirblk;
        de->inode = 2; de->rec_len = 12; de->name_len = 1;
        de->file_type = 0; de->name[0] = '.';

        struct dirent2 *de2 = (struct dirent2 *)(dirblk + 12);
        de2->inode = 2; de2->rec_len = 12; de2->name_len = 2;
        de2->file_type = 0; de2->name[0] = '.'; de2->name[1] = '.';

        /* inode 11 的引用计数为 2(自身 . 加上这里的条目),故须在 / 中列出 */
        struct dirent2 *de3 = (struct dirent2 *)(dirblk + 24);
        de3->inode = 11; de3->rec_len = BLOCK_SIZE - 24; de3->name_len = 10;
        de3->file_type = 0;
        memcpy(de3->name, "lost+found", 10);
    }
    pwrite(fd, dirblk, BLOCK_SIZE, (off_t)(data0 + 1) * BLOCK_SIZE);

    /* lost+found :. 和 .. */
    memset(dirblk, 0, BLOCK_SIZE);
    {
        struct dirent2 *de = (struct dirent2 *)dirblk;
        de->inode = 11; de->rec_len = 12; de->name_len = 1;
        de->file_type = 0; de->name[0] = '.';
        struct dirent2 *de2 = (struct dirent2 *)(dirblk + 12);
        de2->inode = 2; de2->rec_len = BLOCK_SIZE - 12; de2->name_len = 2;
        de2->file_type = 0; de2->name[0] = '.'; de2->name[1] = '.';
    }
    pwrite(fd, dirblk, BLOCK_SIZE, (off_t)data0 * BLOCK_SIZE);
    free(dirblk);

    fsync(fd);
    printf("mkfs: done (magic 0x%04X, %lu blocks, %lu groups, "
           "%lu inodes, %lu free blocks)\n",
           EXT2_SUPER_MAGIC, block_count, groups, inodes_count,
           s->s_free_blocks_count);
    return 0;
}

#ifdef MKFS_CLI_MAIN
int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: mkfs <device> [size-mib]\n");
        return 1;
    }
    const char *dev = argv[1];
    int size_mib = (argc > 2) ? atoi(argv[2]) : 0;

    int fd = open(dev, O_WRONLY);
    if (fd < 0) {
        perror(dev);
        return 1;
    }

    long total_bytes;
    if (size_mib > 0)
        total_bytes = (long)size_mib * 1024 * 1024;
    else
        total_bytes = -1;   /* 自动探测:device_size/partition_size/BLKGETSIZE */

    int rc = mkfs_ext2_fd(fd, total_bytes, dev);
    close(fd);
    return rc < 0 ? 1 : 0;
}
#endif /* MKFS_CLI_MAIN */
