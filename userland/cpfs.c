/* cpfs.c - 把当前根(=initramfs 根)的目录树拷进分区 2 的 ext2。
 * install 时调用:cpfs <disk> <root_start_lba> <tot_disk_sec>
 *
 * 布局约定(与 install 的 write_ext2_whole_disk / mkfs.c 完全一致):
 *   4K 块, bpg=32768, ipg=8192, gd_blocks=ceil(groups*32/4096)
 *   0-based 块 0 = 超级块, 块 1 = GDT, 块 2 = block_bitmap,
 *   块 3 = inode_bitmap, 块 4..(4+itable-1) = inode 表,
 *   块 meta 起 = 数据区(meta = 4 + itable_blocks, gd_blocks=1 时)
 *
 * 块号约定(与 mkfs.c / install.c 一致, host 实测):
 *   - 物理位置(分区内 0-based 块号):inode 表、位图、数据块、目录块
 *     全部按 0-based 块号定位。
 *   - inode i_block[0..11] 的 12 个直接块:0-based 块号(0 = 无块);
 *     一级/二级间接块内 1024 个指针:0-based 块号。
 *
 * rev-0 ext2 语义:
 *   - 目录条目 file_type 字段保留 0(ext3 才有 1=reg/2=dir 标志)。
 *   - 目录 inode i_size 按 4K 块对齐(已占块数*4096),
 *     i_blocks 单位 512B 扇区(块数*8)。
 *   - 目录 i_links_count = 子目录数 + 1(root) / + 2(非 root,含 ..)。
 *
 * 约束:
 *   - 整盘 pwrite(本内核 /dev/<disk>2 未注册,mount 会阻塞),
 *     偏移 = part_off + 块号*4096,块号相对分区 0。
 *   - 数据块顺序分配(从组 0 首数据块起,跳组内元数据区),
 *     文件 >48KB 走一级间接(≤4MB)、>4MB 走二级间接。
 *   - 每个目录块在 add_dirent 前才分配(避免目录条目丢失),
 *     条目先写进父目录,数据块后分配,顺序保证一致。
 *   - 挂载点(相对根 st_dev 不同者,如 devtmpfs 的 /dev、iso9660 的
 *     /cdrom)与 /proc /sys /dev /tmp /run /mnt /root /parlz /install.d:
 *     **只建同名空目录, 不递归**。递归进 /cdrom 会把整张安装 ISO
 *     (fat16.img 64 MiB + vmlinuz 37 MiB)当 rootfs 拷 → 中途失败,
 *     目标分区只剩半个根(实测装完停在 initramfs)。
 *   - 符号链接**必须照拷**(ext2 symlink inode):initramfs 里 384 个
 *     软链 vs 80 个真文件, busybox applet 与 /bin/busybox 全是软链,
 *     丢了就等于把命令表清空。字符/块设备、socket、fifo 跳过。
 *   - inode 表 + 位图预载(保住 install 写好的 ino2/ino11),
 *     最后一次性回写。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#define EXT2_BLOCK  4096
#define INODE_SIZE  128
#define BPG         32768
#define IPG         8192
#define MAX_INO     8192
#define DIR_BLOCKS_MAX 1024

static int  dfd;
static off_t part_off;
static unsigned long part_blocks;
static unsigned long gd_blocks;
static unsigned long itable_blocks;
static unsigned long meta;
static unsigned long data0_block;
static unsigned long part_groups;
static unsigned long itab_block;

static unsigned char *itab;       /* inode 表镜像(全组, 连续块) */
static unsigned char *blk_bmp;    /* 块位图(全组, 内存记账) */
static unsigned char *ino_bmp;    /* inode 位图(全组, 内存记账) */

static unsigned long next_ino = 12;
static unsigned long next_blk;

/* 根的 st_dev: 与其不同的目录就是挂载点(devtmpfs/iso9660/tmpfs/…),
 * 只建同名空目录, 不往下走。main 里 stat("/") 赋值。 */
static dev_t root_dev;

struct ext2_inode {
    unsigned short  i_mode;
    unsigned short  i_uid;
    unsigned int    i_size_lo;
    unsigned int    i_atime;
    unsigned int    i_ctime;
    unsigned int    i_mtime;
    unsigned int    i_dtime;
    unsigned short  i_gid;
    unsigned short  i_links_count;
    unsigned int    i_blocks_lo;
    unsigned int    i_flags;
    unsigned int    i_osd1;
    unsigned int    i_block[15];
    unsigned int    i_generation;
    unsigned int    i_file_acl_lo;
    unsigned int    i_size_high;
    unsigned int    i_obso_faddr;
    unsigned char   i_osd2[12];
} __attribute__((packed));

struct dirext2 {
    unsigned int  inode;
    unsigned short rec_len;
    unsigned char  name_len;
    unsigned char  file_type;
    char           name[];
} __attribute__((packed));

static struct ext2_inode *ino_at(unsigned long n)
{
    return (struct ext2_inode *)(itab + (n - 1) * INODE_SIZE);
}

/* 位图字节 popcount(不依赖 GCC 内建, 静态用户空间兼容性) */
static int popcount_byte(unsigned char v)
{
    int c = 0;
    while (v) {
        c += v & 1;
        v >>= 1;
    }
    return c;
}

/* 0-based 块号 -> 组内相对块号 */
static unsigned long blk_rel(unsigned long b)
{
    unsigned long g = b / BPG;
    return b - g * BPG;
}

static unsigned long group_data_start(unsigned long g)
{
    return g * BPG + meta;
}

static unsigned long group_data_end(unsigned long g)
{
    unsigned long last = (g == part_blocks / BPG && part_blocks % BPG)
        ? part_blocks : (g + 1) * BPG;
    return last;
}

/* 分配下一个 0-based 数据块, 尊重预载的 blk_bmp(install 占用的块
 * 在位图里已置位, 不能重认领, 否则 ino2/ino11/新 inode 共享物理块)。 */
static unsigned long alloc_blk(void)
{
    for (;;) {
        if (next_blk < group_data_start(next_blk / BPG))
            next_blk = group_data_start(next_blk / BPG);
        if (next_blk >= group_data_end(next_blk / BPG)) {
            next_blk++;
            continue;
        }
        unsigned long g = next_blk / BPG;
        unsigned long rel = blk_rel(next_blk);
        if (blk_bmp[g * EXT2_BLOCK + (rel >> 3)] & (1u << (rel & 7))) {
            next_blk++;
            continue;
        }
        blk_bmp[g * EXT2_BLOCK + (rel >> 3)] |= 1u << (rel & 7);
        return next_blk++;
    }
}

static unsigned long alloc_ino(void)
{
    unsigned long n = next_ino++;
    if (n >= MAX_INO) {
        fprintf(stderr, "cpfs: too many inodes\n");
        exit(1);
    }
    ino_bmp[(n - 1) >> 3] |= 1u << ((n - 1) & 7);
    return n;
}

static int flush_itab(void)
{
    if (pwrite(dfd, itab, itable_blocks * EXT2_BLOCK,
               part_off + (off_t)itab_block * EXT2_BLOCK)
            != (ssize_t)(itable_blocks * EXT2_BLOCK)) {
        fprintf(stderr, "cpfs: pwrite inode table failed\n");
        return -1;
    }
    return 0;
}

/* 初始化 inode: mode/size/链接数/直接块。写后即刷 inode 表落盘
 * (i_blocks/直接块后续由 write_file_data/add_dirent 增量回填)。 */
static void set_inode(unsigned long n, unsigned short mode,
                      unsigned long size, unsigned long links,
                      unsigned long blk0based)
{
    struct ext2_inode *i = ino_at(n);
    memset(i, 0, INODE_SIZE);
    i->i_mode = mode;
    i->i_size_lo = (unsigned int)size;
    i->i_links_count = (unsigned short)links;
    i->i_blocks_lo = 0;
    if (blk0based)
        i->i_block[0] = (unsigned int)blk0based;   /* 0-based 物理块号 */
    flush_itab();
}

/* 写符号链接 inode(0777 | S_IFLNK)。
 * ext2 快链: 目标 < 60 字节时直接放进 i_block 那 60 字节, i_blocks=0
 * —— 内核 __ext2_read_inode 正是**按 i_blocks 是否为 0** 区分快/慢链,
 * 所以这里必须留 0, 不能顺手记 8 扇区。慢链才分配一个数据块。
 * busybox applet 软链全走快链(目标最长 /sbin/busybox = 13 字节)。 */
static int write_symlink(unsigned long n, const char *target)
{
    size_t len = strlen(target);
    struct ext2_inode *i = ino_at(n);

    memset(i, 0, INODE_SIZE);
    i->i_mode = 0xA1FF;
    i->i_size_lo = (unsigned int)len;
    i->i_links_count = 1;

    if (len < sizeof i->i_block) {
        memcpy((char *)i->i_block, target, len);
    } else {
        unsigned char *blk = calloc(1, EXT2_BLOCK);
        if (!blk)
            return -1;
        memcpy(blk, target, len);
        unsigned long b = alloc_blk();
        if (pwrite(dfd, blk, EXT2_BLOCK,
                   part_off + (off_t)b * EXT2_BLOCK) != EXT2_BLOCK) {
            free(blk);
            return -1;
        }
        free(blk);
        i->i_block[0] = (unsigned int)b;
        i->i_blocks_lo = 8;
    }
    return flush_itab();
}

/* 往 inode n 的目录第 idx 个数据块(0-based 序号)分配/取块号。
 * 序号 0..11 存 i_block[0..11]; 序号 >=12 走 i_block[12] 一级间接
 * (1024 个 0-based 指针, 第 idx-12 项)。新块先清零写盘。 */
static unsigned long get_dir_block(unsigned long n, unsigned long idx,
                                   int alloc)
{
    struct ext2_inode *i = ino_at(n);
    if (idx < 12) {
        if (alloc && !i->i_block[idx])
            i->i_block[idx] = (unsigned int)alloc_blk();
        return (unsigned long)i->i_block[idx];
    }
    if (alloc && !i->i_block[12]) {
        i->i_block[12] = (unsigned int)alloc_blk();
        unsigned char *zero = calloc(1, EXT2_BLOCK);
        if (zero) {
            pwrite(dfd, zero, EXT2_BLOCK,
                   part_off + (off_t)i->i_block[12] * EXT2_BLOCK);
            free(zero);
        }
    }
    unsigned char *l1 = malloc(EXT2_BLOCK);
    if (!l1)
        return 0;
    pread(dfd, l1, EXT2_BLOCK,
          part_off + (off_t)i->i_block[12] * EXT2_BLOCK);
    unsigned int *slot = (unsigned int *)l1 + (idx - 12);
    if (alloc && !*slot)
        *slot = (unsigned int)alloc_blk();
    pwrite(dfd, l1, EXT2_BLOCK,
           part_off + (off_t)i->i_block[12] * EXT2_BLOCK);
    unsigned long b = *slot;
    free(l1);
    return b;
}

/* 目录 inode 的写位置状态(块序号 + 块内偏移), 按 inode 号索引,
 * 递归嵌套目录互不干扰。 */
static unsigned long dir_off_arr[MAX_INO];
static unsigned long dir_blk_arr[MAX_INO];

/* 续块: 当前块(idx = dir_blk_arr[n])写满后取/分配 idx+1,
 * 新块清零写盘, 写位置归零。 */
static int dir_next_block(unsigned long n)
{
    unsigned long idx = dir_blk_arr[n];
    if (idx + 1 >= DIR_BLOCKS_MAX) {
        fprintf(stderr, "cpfs: inode %lu dir block overflow\n", n);
        return -1;
    }
    unsigned long b = get_dir_block(n, idx + 1, 1);
    if (!b)
        return -1;
    unsigned char *zero = calloc(1, EXT2_BLOCK);
    if (zero) {
        pwrite(dfd, zero, EXT2_BLOCK, part_off + (off_t)b * EXT2_BLOCK);
        free(zero);
    }
    dir_off_arr[n] = 0;
    dir_blk_arr[n] = idx + 1;
    return 0;
}

/* 往 inode n 的目录追加条目(child_ino, name)。
 * 写到第 dir_blk_arr[n] 个块的 dir_off_arr[n] 处, 块满自动续块。
 * rev-0 ext2: 条目 file_type 字段保留 0; i_size/i_blocks 按块对齐更新。
 * 条目直接用 4 字节边界手写, 不依赖结构体布局。 */
static int add_dirent(unsigned long n, unsigned long child_ino,
                      const char *name)
{
    unsigned long len = strlen(name);
    unsigned long padded = (len + 3) & ~3u;
    unsigned long elen = 8 + padded;

    if (dir_off_arr[n] + elen > EXT2_BLOCK)
        if (dir_next_block(n) < 0)
            return -1;

    unsigned long cur = get_dir_block(n, dir_blk_arr[n], 1);
    if (!cur)
        return -1;

    unsigned char *blk = malloc(EXT2_BLOCK);
    if (!blk)
        return -1;
    pread(dfd, blk, EXT2_BLOCK, part_off + (off_t)cur * EXT2_BLOCK);

    /* 逐条走完 rec_len 链找最后一条目的块内偏移, 把它的 rec_len
     * 截断到 dir_off_arr[n](新条目起始位置) */
    unsigned long last_off = 0;
    {
        unsigned char *p = blk;
        unsigned long off2 = 0;
        while (off2 < EXT2_BLOCK) {
            unsigned long ino_v = p[0] | ((unsigned long)p[1] << 8)
                               | ((unsigned long)p[2] << 16)
                               | ((unsigned long)p[3] << 24);
            unsigned long rec = p[4] | ((unsigned long)p[5] << 8);
            if (ino_v == 0 || rec == 0)
                break;
            last_off = off2;
            off2 += rec;
            if (off2 >= EXT2_BLOCK)
                break;
            p += rec;
        }
    }
    /* 追加新条目前, 若本块已满(dir_off_arr >= 块长)需先经
     * dir_next_block 分配下一块; 上面的 dir_next_block 检查已保证
     * cur 块内还有位置, 否则 add_dirent 返回 1 让调用方继续。 */
    /* 截断最后一条目的 rec_len: 若 last_off < 写位置, 缩短它 */
    if (last_off < dir_off_arr[n]) {
        blk[last_off + 4] = (unsigned char)((dir_off_arr[n] - last_off)
                                            & 0xFF);
        blk[last_off + 5] = (unsigned char)(((dir_off_arr[n] - last_off)
                                              >> 8) & 0xFF);
    }

    /* 写新条目(4 字节边界, 显式字节序, 不依赖结构体) */
    unsigned long w = dir_off_arr[n];
    unsigned char *e = blk + w;
    e[0] = (unsigned char)(child_ino & 0xFF);
    e[1] = (unsigned char)((child_ino >> 8) & 0xFF);
    e[2] = (unsigned char)((child_ino >> 16) & 0xFF);
    e[3] = (unsigned char)((child_ino >> 24) & 0xFF);
    e[4] = (unsigned char)((EXT2_BLOCK - w) & 0xFF);
    e[5] = (unsigned char)(((EXT2_BLOCK - w) >> 8) & 0xFF);
    e[6] = (unsigned char)len;
    e[7] = 0;   /* file_type: rev-0 ext2 保留 0 */
    memset(e + 8, 0, padded);
    memcpy(e + 8, name, len);
    dir_off_arr[n] = w + elen;

    pwrite(dfd, blk, EXT2_BLOCK, part_off + (off_t)cur * EXT2_BLOCK);
    free(blk);

    /* 目录 inode 计数(rev-0 ext2 按 4K 块对齐) */
    unsigned long nblk = dir_blk_arr[n] + 1;
    ino_at(n)->i_size_lo = (unsigned int)(nblk * EXT2_BLOCK);
    ino_at(n)->i_blocks_lo = (unsigned int)(nblk * 8);

    return 0;
}

/* 初始化子目录块: 写 "." 与 ".." 两条目(各 12 字节, 共 24 字节)。
 * rev-0 ext2: file_type 字段保留 0。条目显式按字节写(4 字节边界)。 */
static int write_dir_base(unsigned long cino, unsigned long parent,
                          unsigned long cblk)
{
    unsigned char base[EXT2_BLOCK];
    memset(base, 0, EXT2_BLOCK);
    /* "." : off 0 */
    base[0] = (unsigned char)(cino & 0xFF);
    base[1] = (unsigned char)((cino >> 8) & 0xFF);
    base[2] = (unsigned char)((cino >> 16) & 0xFF);
    base[3] = (unsigned char)((cino >> 24) & 0xFF);
    base[4] = 12; base[5] = 0;          /* rec_len = 12 */
    base[6] = 1;                        /* name_len */
    base[7] = 0;                        /* file_type: rev-0 保留 0 */
    base[8] = '.';
    /* "..": off 12 */
    base[12] = (unsigned char)(parent & 0xFF);
    base[13] = (unsigned char)((parent >> 8) & 0xFF);
    base[14] = (unsigned char)((parent >> 16) & 0xFF);
    base[15] = (unsigned char)((parent >> 24) & 0xFF);
    base[16] = (EXT2_BLOCK - 12) & 0xFF;
    base[17] = ((EXT2_BLOCK - 12) >> 8) & 0xFF;
    base[18] = 2;
    base[19] = 0;
    base[20] = '.'; base[21] = '.';
    if (pwrite(dfd, base, EXT2_BLOCK, part_off + (off_t)cblk * EXT2_BLOCK)
            != EXT2_BLOCK)
        return -1;
    ino_at(cino)->i_size_lo = EXT2_BLOCK;
    ino_at(cino)->i_blocks_lo = 8;
    dir_off_arr[cino] = 24;
    dir_blk_arr[cino] = 0;
    return 0;
}

/* 写文件数据到 inode n。0-based 块号由 alloc_blk 分配。
 *
 * 块指针布局(与 mkfs.c 一致, 全部 0-based 分区相对块号):
 *   - 数据块 1..12(0-based 序号 0..11) 存 i_block[0..11] 直接指针;
 *   - 第 13 个数据块起走一级间接: L1 段号 = (序号-12)/1024,
 *     槽 = (序号-12)%1024;
 *   - 仅一个 L1 段时 i_block[12] = L1 段 0 块号;
 *   - 多个 L1 段(>4MB)建二级块 L2, i_block[13] = L2 块号,
 *     L2 块内按序存 L1 段块号(此时 i_block[12] 必须为 0,
 *     内核按 L2[i_block[13]][slot] 寻址, 双写 i_block[12] 会
 *     让 ext2 读器混淆)。
 * i_size = 实际字节数(stat st_size); i_blocks = 实占块数*8 扇区
 * (含数据块 + L1 段块 + L2 块)。
 *
 * 关键约定: L1/L2 块号在分配时即时写入 inode 表并落盘 —— 不能
 * 延迟到循环后统一写, 否则整表回写交错窗口会把 L1 段 0 指针
 * 冲成 0(表现为 >48KB 文件经 L1 读回全 0)。直接块指针在循环
 * 内写内存 itab, 由本函数末尾统一回写。 */
static int write_file_data(unsigned long n, const char *path,
                           unsigned long fsize)
{
    unsigned long nb = (fsize + EXT2_BLOCK - 1) / EXT2_BLOCK;
    if (nb == 0)
        return 0;    /* 空文件 */
    if (nb > 16 * 1024) {
        fprintf(stderr, "cpfs: %s: %lu bytes too big\n", path, fsize);
        return -1;
    }
    unsigned char *buf = malloc(EXT2_BLOCK);
    if (!buf)
        return -1;
    int sfd = open(path, O_RDONLY);
    if (sfd < 0) {
        fprintf(stderr, "cpfs: open %s: %s\n", path, strerror(errno));
        free(buf);
        return -1;
    }
    unsigned char *l1 = calloc(16, EXT2_BLOCK);
    unsigned int l1ids[16];
    unsigned long l1cnt = 0;
    unsigned long written = 0;
    unsigned long l2_block = 0;    /* 二级块号(若有), 循环内 alloc */

    while (written < nb) {
        /* 块序号 written(0-based, 第 1 块 = 0)。
         * 直接块 written=0..11 -> i_block[0..11]。
         * 一级间接:written>=12(第 13 块)起, L1 段号=(written-12)/1024,
         * 槽=(written-12)%1024。进入新 L1 段先分配 L1 块并清零占位
         * (块位图置位), 再分配数据块 b —— L1 物理块号严格小于其
         * 指向的数据块号。L1 段块号回填 inode 在循环外统一做
         * (有 L2 时 i_block[12]=0, 单 L1 段时 i_block[12]=段 0)。 */
        if (written >= 12) {
            unsigned long l1i = (written - 12) / 1024;
            if (l1i >= l1cnt) {
                if (l1i >= 14)
                    break;
                unsigned long lb = alloc_blk();
                l1ids[l1i] = (unsigned int)lb;
                l1cnt = l1i + 1;
                memset(l1 + l1i * EXT2_BLOCK, 0, EXT2_BLOCK);
                pwrite(dfd, l1 + l1i * EXT2_BLOCK, EXT2_BLOCK,
                       part_off + (off_t)lb * EXT2_BLOCK);
                /* 一级段块号立即落 inode 表: 写 L1 段 0 的块号进
                 * i_block[12] 并刷盘 —— 这是防止 L1 指针在后续整表
                 * 回写中被旧值覆盖的关键锚点(段 0 号在 >4MB 建 L2 前
                 * 必须先可见)。 */
                if (l1i == 0 && l1cnt == 1) {
                    ino_at(n)->i_block[12] = (unsigned int)lb;
                    flush_itab();
                }
            }
        }
        unsigned long b = alloc_blk();
        unsigned long base_off = written * EXT2_BLOCK;
        memset(buf, 0, EXT2_BLOCK);
        if (fsize > base_off) {
            unsigned long take = fsize - base_off;
            if (take > EXT2_BLOCK)
                take = EXT2_BLOCK;
            if (pread(sfd, buf, take, (off_t)base_off) != (ssize_t)take)
                memset(buf, 0, EXT2_BLOCK);
        }
        if (pwrite(dfd, buf, EXT2_BLOCK, part_off + (off_t)b * EXT2_BLOCK)
                != (ssize_t)EXT2_BLOCK) {
            fprintf(stderr, "cpfs: pwrite data blk %lu failed: %s\n",
                    b, strerror(errno));
            free(l1);
            free(buf);
            close(sfd);
            return -1;
        }
        if (written < 12)
            ino_at(n)->i_block[written] = (unsigned int)b;
        else
            ((unsigned int *)l1 + ((written - 12) / 1024) * 1024)
                [(written - 12) % 1024] = (unsigned int)b;
        written++;
    }
    if (written < nb) {
        fprintf(stderr, "cpfs: %s: only wrote %lu/%lu blocks\n",
                path, written, nb);
        free(l1);
        free(buf);
        close(sfd);
        return -1;
    }
    /* 一级块内容已全填(循环内按段分配并占位), 刷所有 L1 段;
     * >4MB 时建 L2 段汇总 L1 块号。 */
    for (unsigned long k = 0; k < l1cnt; k++)
        pwrite(dfd, l1 + k * EXT2_BLOCK, EXT2_BLOCK,
               part_off + (off_t)l1ids[k] * EXT2_BLOCK);
    unsigned long l2b = 0;
    if (l1cnt > 1) {
        unsigned char l2[EXT2_BLOCK];
        memset(l2, 0, EXT2_BLOCK);
        /* L2 只存 L1 段 1..(N-1): 段 0 固定存 i_block[12](内核对文件块
         * 12..1035 走一级直连读, 不经 L2), 重复写入 L2 会使段 0 的
         * 数据块被读端按 L2[0] 二次寻址而错位。 */
        for (unsigned long k2 = 1; k2 < l1cnt; k2++)
            ((unsigned int *)l2)[k2 - 1] = l1ids[k2];
        l2b = alloc_blk();
        pwrite(dfd, l2, EXT2_BLOCK, part_off + (off_t)l2b * EXT2_BLOCK);
    }
    /* 块指针回填(与内核 ext2_block_to_path 一致, 4K 块):
     *   i_block[12] = L1 段 0(单一级块, 直接指向), 覆盖文件块 12..1035;
     *   仅当 L1 段数 >1(>4MB)时 i_block[13] = L2 块, 覆盖文件块
     *   1036..起; L2 块内按序存 L1 段 1..N 块号(段 0 已存 i_block[12],
     *   不进 L2)。双段时 i_block[12] 保持 L1 段 0, 不能置 0
     *   (文件块 12..1035 仍走一级直连 i_block[12] 读, 内核不按 L2 取)。 */
    if (l1cnt >= 1) {
        ino_at(n)->i_block[12] = l1ids[0];
        if (l1cnt > 1) {
            ino_at(n)->i_block[13] = (unsigned int)l2b;
        }
    }
    /* i_blocks(512B 扇区数)= 数据块 + L1 段数 + L2 块(若有) */
    {
        unsigned long nblk = written + l1cnt;
        if (l1cnt > 1)
            nblk += 1;
        ino_at(n)->i_blocks_lo = (unsigned int)(nblk * 8);
    }
    free(l1);
    free(buf);
    close(sfd);
    /* 所有数据块/L1 段写完后一次性回写 inode 表(直接块指针
     * i_block[0..11] 在循环内只写内存, 此处随 L1/L2 指针一起落盘),
     * 保证本文件全部块指针 + i_blocks 原子生效 */
    int rc = flush_itab();
    return rc;
}

/* 递归拷贝目录: 把 path 目录下条目写进 dir_ino 的目录块链。
 * 每个 inode 有独立的写位置状态, 递归嵌套互不干扰。
 * 挂载点(st_dev 与根不同)与 mount_dirs 里的顶层目录: 目标根里
 * 建成同名**空目录**后就不再往下走 —— 换根后 init 要往这些点挂
 * devtmpfs/proc/sysfs/tmpfs, 目录必须在, 但内容不属于 rootfs。 */
static const char *const mount_dirs[] = {
    "proc", "sys", "dev", "tmp", "run", "mnt", "cdrom", NULL
};

static int is_mount_dir(const char *nm)
{
    for (int i = 0; mount_dirs[i]; i++)
        if (strcmp(nm, mount_dirs[i]) == 0)
            return 1;
    return 0;
}

static int copy_dir(const char *path, unsigned long dir_ino, int depth)
{
    DIR *dp = opendir(path[0] ? path : ".");
    if (!dp) {
        fprintf(stderr, "cpfs: opendir %s: %s\n",
                path[0] ? path : "(root)", strerror(errno));
        return -1;
    }

    struct dirent *de;
    unsigned long nsub = 0;
    while ((de = readdir(dp))) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0)
            continue;
        /* ISO 安装介质的自动安装触发器: 装好的盘里绝不能有它,
         * 否则下次启动又重跑一遍安装。 */
        if (depth == 0 && strcmp(nm, "install.d") == 0)
            continue;
        char child[512];
        int clen = snprintf(child, sizeof child, "%s%s%s",
                            path[0] ? path : "",
                            path[0] ? "/" : "", nm);
        if (clen < 0 || clen >= (int)sizeof child)
            continue;
        struct stat st;
        if (lstat(child, &st) < 0)
            continue;
        if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) ||
            S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode))
            continue;
        if (S_ISDIR(st.st_mode)) {
            int walk = !(depth == 0 && is_mount_dir(nm));
            if (st.st_dev != root_dev)
                walk = 0;   /* 嵌套挂载点: 建空目录即可 */
            unsigned long cino = alloc_ino();
            unsigned long cblk = alloc_blk();
            /* 目录 inode 先初始化(links=2: "." + 父目录的".."条目),
             * i_size/i_blocks 由 write_dir_base / add_dirent 回填 */
            set_inode(cino, 0x41ED, 0, 2, cblk);
            /* 顺序: 条目先写进父目录, 再初始化子目录基块, 最后递归 */
            if (add_dirent(dir_ino, cino, nm) < 0) {
                fprintf(stderr, "cpfs: add_dirent(dir %s) failed\n", child);
                return -1;
            }
            if (write_dir_base(cino, dir_ino, cblk) < 0) {
                fprintf(stderr, "cpfs: write_dir_base(%s) failed\n", child);
                return -1;
            }
            if (walk && copy_dir(child, cino, depth + 1) < 0)
                return -1;
            /* 链接数 = 2 + 子目录总数。2 = 本目录的 "." 与
             * ".."(root 的 ".." 指向自身, 多一次引用)。
             * 子目录总数: root = cpfs 建的 nsub + lost+found(预载, 1);
             * 非 root = cpfs 建的 nsub。 */
            nsub++;
            if (depth == 0)
                ino_at(dir_ino)->i_links_count =
                    (unsigned short)(nsub + 3);   /* 2 + nsub + 1 */
            else
                ino_at(dir_ino)->i_links_count =
                    (unsigned short)(nsub + 2);
            flush_itab();
        } else if (S_ISREG(st.st_mode)) {
            unsigned long cino = alloc_ino();
            unsigned long nb =
                (st.st_size + EXT2_BLOCK - 1) / EXT2_BLOCK;
            /* 保留源文件权限(0644/0755 等): 全量 rootfs 拷贝里 bin/ 下
             * 是 ELF 可执行位 0755, 硬编码 0x81A4(0644) 会让 mount 后
             * ./xxx 全报 "not found or not executable"(execve 需 x 位)。 */
            unsigned short mode = 0x81A4;
            unsigned int fm = (unsigned int)st.st_mode;
            mode = (unsigned short)(0x8000 | (fm & 01777));
            set_inode(cino, mode, st.st_size, 1, 0);
            /* 顺序: 条目先写进父目录, 数据块后分配(保证 add_dirent
             * 时 next_blk 不被推高导致目录块错位) */
            if (add_dirent(dir_ino, cino, nm) < 0) {
                fprintf(stderr, "cpfs: add_dirent(file %s) failed\n", child);
                return -1;
            }
            if (write_file_data(cino, child, st.st_size) < 0) {
                fprintf(stderr, "cpfs: write_file_data(%s) failed\n", child);
                return -1;
            }
        } else if (S_ISLNK(st.st_mode)) {
            char tgt[512];
            ssize_t tl = readlink(child, tgt, sizeof tgt - 1);
            if (tl < 0) {
                fprintf(stderr, "cpfs: readlink %s: %s\n",
                        child, strerror(errno));
                return -1;
            }
            if ((size_t)tl == sizeof tgt - 1) {
                fprintf(stderr, "cpfs: %s: 链接目标过长, 拒绝截断\n", child);
                return -1;
            }
            tgt[tl] = '\0';
            unsigned long cino = alloc_ino();
            if (add_dirent(dir_ino, cino, nm) < 0) {
                fprintf(stderr, "cpfs: add_dirent(link %s) failed\n", child);
                return -1;
            }
            if (write_symlink(cino, tgt) < 0) {
                fprintf(stderr, "cpfs: write_symlink(%s) failed\n", child);
                return -1;
            }
        }
    }
    closedir(dp);
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "usage: cpfs <disk> <root_start_lba> <tot_sec>\n");
        return 1;
    }
    dfd = open(argv[1], O_RDWR);
    if (dfd < 0) {
        perror("cpfs: open disk");
        return 1;
    }
    part_off = (off_t)atol(argv[2]) * 512;
    unsigned long tot_sec = atol(argv[3]);

    /* 分区 2 = 整盘总扇区 - 83969(LBA 0 + boot 分区 81920 + LBA 1 标记) */
    unsigned long root_sec = (tot_sec > 83969) ? (tot_sec - 83969) : 0;
    part_blocks = root_sec * 512 / EXT2_BLOCK;
    if (part_blocks < 4096) {
        fprintf(stderr, "cpfs: partition 2 too small (%lu blocks)\n",
                part_blocks);
        return 1;
    }

    part_groups = (part_blocks + BPG - 1) / BPG;
    if (part_groups < 1)
        part_groups = 1;
    itable_blocks = (IPG * INODE_SIZE + EXT2_BLOCK - 1) / EXT2_BLOCK;
    part_groups = (part_blocks + BPG - 1) / BPG;
    if (part_groups < 1)
        part_groups = 1;
    /* 与 mkfs.c 一致: 位图按 part_groups 计, inode 表按 part_blocks 计
     * (组 0 位图 8192 ino 槽; ipg>8192 的组 1.. 在 8M 分区下不存在)。
     * 组 0 之外若仍有块, inode 表块数按实际组数补齐。 */
    gd_blocks = (part_groups * 32 + EXT2_BLOCK - 1) / EXT2_BLOCK;
    meta = 4 + itable_blocks;
    data0_block = meta;
    next_blk = data0_block + 2;
    itab_block = 3 + gd_blocks;

    itab = malloc(itable_blocks * EXT2_BLOCK);
    blk_bmp = malloc(part_groups * EXT2_BLOCK);
    ino_bmp = malloc(part_groups * EXT2_BLOCK);
    if (!itab || !blk_bmp || !ino_bmp) {
        fprintf(stderr, "cpfs: malloc failed\n");
        close(dfd);
        return 1;
    }

    /* 预载 GDT 取 inode 表 0-based 块号(比本地公式更可靠) */
    {
        unsigned char gdt[32];
        if (pread(dfd, gdt, 32, part_off + 4096) != 32) {
            fprintf(stderr, "cpfs: pread GDT failed\n");
            close(dfd);
            return 1;
        }
        unsigned int gdt_itb;
        memcpy(&gdt_itb, gdt + 8, 4);
        if (gdt_itb)
            itab_block = gdt_itb;
    }
    /* 预载 inode 表(组 0)与位图(全部组), 保住 ino2/ino11 */
    if (pread(dfd, itab, itable_blocks * EXT2_BLOCK,
              part_off + (off_t)itab_block * EXT2_BLOCK)
            != (ssize_t)(itable_blocks * EXT2_BLOCK)) {
        fprintf(stderr, "cpfs: pread existing inode table failed\n");
        close(dfd);
        return 1;
    }
    for (unsigned long g = 0; g < part_groups; g++) {
        if (pread(dfd, blk_bmp + g * EXT2_BLOCK, EXT2_BLOCK,
                  part_off + (off_t)(g * BPG + 1 + gd_blocks) * EXT2_BLOCK)
                != EXT2_BLOCK) {
            fprintf(stderr, "cpfs: pread block bitmap %lu failed\n", g);
            close(dfd);
            return 1;
        }
        if (pread(dfd, ino_bmp + g * EXT2_BLOCK, EXT2_BLOCK,
                  part_off + (off_t)(g * BPG + 2 + gd_blocks) * EXT2_BLOCK)
                != EXT2_BLOCK) {
            fprintf(stderr, "cpfs: pread inode bitmap %lu failed\n", g);
            close(dfd);
            return 1;
        }
    }
    /* 预载位图基线 popcount(= mkfs + install 已置位总数, 含 padding 位)。
     * 超级块汇总的增量 = 写树后的 popcount - 此基线。
     * 基线里已登记的目录数 = 当前 inode 表(预载)中 1..next_ino-1 的
     * 目录 inode 总数(mkfs 建的 ino2/ino11 + install 写入的目录)。
     * cpfs 之后 bg_used_dirs = 全目录总数 - 基线已登记目录数。 */
    unsigned long base_blk_pop = 0, base_ino_pop = 0, base_dir_cnt = 0;
    for (unsigned long g = 0; g < part_groups; g++) {
        for (int b = 0; b < EXT2_BLOCK; b++) {
            base_blk_pop += popcount_byte(blk_bmp[g * EXT2_BLOCK + b]);
            base_ino_pop += popcount_byte(ino_bmp[g * EXT2_BLOCK + b]);
        }
    }
    for (unsigned long n = 1; n < next_ino; n++)
        if ((ino_at(n)->i_mode & 0xF000) == 0x4000)
            base_dir_cnt++;
    /* 根目录写位置: 预载的 ino2 根目录有 . .. lost+found 三条目
     * (24 字节), 追加位从 off=48 起; 无 lost+found 时为 off=24 */
    unsigned long off_base;
    {
        unsigned char *pd = malloc(EXT2_BLOCK);
        if (!pd) {
            fprintf(stderr, "cpfs: malloc root block failed\n");
            close(dfd);
            return 1;
        }
        pread(dfd, pd, EXT2_BLOCK, part_off + (data0_block + 1) * EXT2_BLOCK);
        struct dirext2 *pe = (struct dirext2 *)(pd + 24);
        if (pe->name_len != 10) {
            off_base = 24;
        } else {
            /* 截断 lost+found 条目(rec_len=4096-24)为自身 24 */
            pe->rec_len = 24;
            pwrite(dfd, pd, EXT2_BLOCK,
                   part_off + (data0_block + 1) * EXT2_BLOCK);
            off_base = 48;
        }
        free(pd);
    }

    /* 根目录写位置状态初始化 */
    dir_off_arr[2] = off_base;
    dir_blk_arr[2] = 0;

    /* 源根的 st_dev(拷贝对象是 cwd —— install 先 chdir("/") 再 exec,
     * 宿主 oracle 则直接 cd 到解包的 initramfs 根):
     * 用它识别挂载点, 见 copy_dir。 */
    {
        struct stat rst;
        if (stat(".", &rst) != 0) {
            perror("cpfs: stat .");
            close(dfd);
            return 1;
        }
        root_dev = rst.st_dev;
    }

    if (copy_dir("", 2, 0) < 0) {
        fprintf(stderr, "cpfs: copy failed\n");
        close(dfd);
        return 1;
    }

    /* 回写 inode 表(组 0) */
    pwrite(dfd, itab, itable_blocks * EXT2_BLOCK,
           part_off + (off_t)itab_block * EXT2_BLOCK);
    /* 回写块/inode 位图(各组) */
    for (unsigned long g = 0; g < part_groups; g++) {
        pwrite(dfd, blk_bmp + g * EXT2_BLOCK, EXT2_BLOCK,
               part_off + (off_t)(g * BPG + 1 + gd_blocks) * EXT2_BLOCK);
        pwrite(dfd, ino_bmp + g * EXT2_BLOCK, EXT2_BLOCK,
               part_off + (off_t)(g * BPG + 2 + gd_blocks) * EXT2_BLOCK);
    }
    /* 更新超级块组汇总: 位图差值补新占用的块/inode(组 0 bg_ 同步)。
     * 超级块在分区块 0 内偏移 1024(4K 块), 只读/写 1024..4095,
     * 不能整块写(会抹块 0 的 0..1023 引导区)。
     * 字段偏移: 超级块 s_free_blocks_count=12, s_free_inodes_count=16;
     * GDT bg_free_blocks=12(u16), bg_free_inodes=14(u16),
     * bg_used_dirs=16(u16)。增量 = 写树后 popcount - 预载基线。 */
    {
        unsigned long cur_blk_pop = 0, cur_ino_pop = 0;
        for (unsigned long g = 0; g < part_groups; g++) {
            for (int b = 0; b < EXT2_BLOCK; b++) {
                cur_blk_pop += popcount_byte(blk_bmp[g * EXT2_BLOCK + b]);
                cur_ino_pop += popcount_byte(ino_bmp[g * EXT2_BLOCK + b]);
            }
        }
        unsigned long new_blocks = cur_blk_pop - base_blk_pop;
        unsigned long new_inos = cur_ino_pop - base_ino_pop;

        unsigned char sb[3072];
        pread(dfd, sb, 3072, part_off + 1024);
        unsigned long free_blocks, free_inodes;
        memcpy(&free_blocks, sb + 12, 4);      /* s_free_blocks_count */
        memcpy(&free_inodes, sb + 16, 4);       /* s_free_inodes_count */
        free_blocks -= new_blocks;
        free_inodes -= new_inos;
        unsigned int fb1 = (unsigned int)free_blocks;
        unsigned int fi1 = (unsigned int)free_inodes;
        memcpy(sb + 12, &fb1, 4);
        memcpy(sb + 16, &fi1, 4);
        pwrite(dfd, sb, 3072, part_off + 1024);
        /* 组 0 bg 汇总同步(块 1 GDT 第 0 条, 32 字节) */
        unsigned char gdtb[32];
        pread(dfd, gdtb, 32, part_off + 4096);
        unsigned int fg, fi;
        memcpy(&fg, gdtb + 12, 2);             /* bg_free_blocks_count */
        fg -= (unsigned int)new_blocks;
        memcpy(gdtb + 12, &fg, 2);
        memcpy(&fi, gdtb + 14, 2);             /* bg_free_inodes_count */
        fi -= (unsigned int)new_inos;
        memcpy(gdtb + 14, &fi, 2);
        unsigned int gd;
        memcpy(&gd, gdtb + 16, 2);             /* bg_used_dirs_count */
        /* bg_used_dirs = 全目录总数 - 预载基线已登记目录数(避免
         * 把 ino2/ino11 等基线目录重复计入; 基线只覆盖组 0) */
        unsigned long total_dirs = 0;
        for (unsigned long n = 1; n < next_ino; n++)
            if ((ino_at(n)->i_mode & 0xF000) == 0x4000)
                total_dirs++;
        gd += (unsigned int)(total_dirs - base_dir_cnt);
        memcpy(gdtb + 16, &gd, 2);
        pwrite(dfd, gdtb, 32, part_off + 4096);
    }
    fsync(dfd);
    close(dfd);
    printf("cpfs: %lu inodes (12..%lu), %lu data blocks\n",
           next_ino - 12, next_ino - 1,
           next_blk - data0_block - 2);
    return 0;
}
