// SPDX-License-Identifier: GPL-2.0-or-later
/* pkgcore.c - 归档读(ar/tar/cpio)、gzip 解压、安全落盘、版本比较。
 * 见 pkgcore.h 的分工说明。约束(AGENTS.md): 静态、无 system()、
 * 覆盖可执行文件先 unlink 再建、写完显式 chmod。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "pkgcore.h"
#include "miniz_tinfl.h"

unsigned char *pc_slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char *b = malloc(n > 0 ? (size_t)n : 1);
    if (!b) {
        fclose(f);
        return NULL;
    }
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (len)
        *len = n;
    return b;
}

const char *pc_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

/* ============================ gzip ============================ */

int pc_gunzip(const unsigned char *src, long srclen,
              unsigned char **out, long *outlen)
{
    if (!src || srclen < 10 || src[0] != 0x1f || src[1] != 0x8b)
        return -1;
    if (src[2] != 8) {                       /* 只支持 deflate 方法 */
        fprintf(stderr, "pkgcore: gzip 方法 %u 不支持(只支持 deflate=8)\n",
                src[2]);
        return -1;
    }
    unsigned flg = src[3];
    long off = 10;
    if (flg & 0x04) {                        /* FEXTRA */
        if (off + 2 > srclen)
            return -1;
        long xl = (long)src[off] | ((long)src[off + 1] << 8);
        off += 2 + xl;
    }
    if (flg & 0x08) {                        /* FNAME */
        while (off < srclen && src[off] != 0)
            off++;
        off++;
    }
    if (flg & 0x10) {                        /* FCOMMENT */
        while (off < srclen && src[off] != 0)
            off++;
        off++;
    }
    if (flg & 0x02)                          /* FHCRC */
        off += 2;
    if (off >= srclen)
        return -1;

    size_t dlen = 0;
    void *d = tinfl_decompress_mem_to_heap(src + off, (size_t)(srclen - off),
                                           &dlen, 0);
    if (!d)
        return -2;
    *out = (unsigned char *)d;
    *outlen = (long)dlen;
    return 0;
}

int pc_maybe_gunzip(const unsigned char *src, long srclen,
                    unsigned char **out, long *outlen, int *owned)
{
    if (srclen >= 2 && src[0] == 0x1f && src[1] == 0x8b) {
        int r = pc_gunzip(src, srclen, out, outlen);
        if (r == 0)
            *owned = 1;
        return r;
    }
    *out = (unsigned char *)src;
    *outlen = srclen;
    *owned = 0;
    return 0;
}

/* ============================== ar ============================== */
/* GNU ar 常规档: "!<arch>\n" + 每个成员 60 字节头(名字 16、mtime 12、
 * uid 6、gid 6、mode 8、size 10、结尾 "`\n" 2), 数据 2 字节对齐。 */
int pc_ar_member(const unsigned char *src, long srclen, const char *name,
                 const unsigned char **data, long *dlen)
{
    if (!src || srclen < 8 || memcmp(src, "!<arch>\n", 8) != 0)
        return -1;
    long off = 8;
    int found = 0;
    while (off + 60 <= srclen) {
        char aname[17];
        const unsigned char *h = src + off;
        if (h[58] != '`' || h[59] != '\n')
            return -1;                       /* 头不是 ar 头: 档已损坏 */
        memcpy(aname, h, 16);
        aname[16] = 0;
        char *sp = aname + strcspn(aname, " ");
        if (*sp == '/')                        /* GNU "/name" 长名表在这 */
            *sp = 0;
        for (char *p = aname; *p; p++)
            if (*p == '/')
                *p = ' ';
        sp = aname + strlen(aname);
        while (sp > aname && sp[-1] == ' ')
            *--sp = 0;
        char szs[11];
        memcpy(szs, h + 48, 10);
        szs[10] = 0;
        long size = strtol(szs, NULL, 10);
        off += 60;
        if (!strcmp(aname, name)) {
            if (off + size > srclen)
                return -1;
            *data = src + off;
            *dlen = size;
            found = 1;
            break;
        }
        off += size + (size & 1);
    }
    return found ? 0 : -2;
}

/* ============================== tar ============================== */

static long tar_num(const unsigned char *p, int n)
{
    /* GNU 的 base-256 大数(最高位 1)用于超大 size; 常规是八进制 ASCII。 */
    if (n > 0 && (p[0] & 0x80)) {
        long v = 0;
        for (int i = 0; i < n; i++)
            v = (v << 8) | p[i];
        return v;
    }
    char buf[32];
    if (n >= (int)sizeof buf)
        n = (int)sizeof buf - 1;
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    for (int i = 0; i < n; i++)
        if (buf[i] == ' ' || buf[i] == 0)
            buf[i] = 0;
    return strtol(buf, NULL, 8);
}

static void tar_name(char *out, size_t n, const unsigned char *hdr)
{
    /* ustar: prefix + name; 其余格式: 只有 name[100] */
    char name[101];
    memcpy(name, hdr, 100);
    name[100] = 0;
    if (!memcmp(hdr + 257, "ustar", 5) && hdr[345] != 0) {
        char prefix[156];
        memcpy(prefix, hdr + 345, 155);
        prefix[155] = 0;
        prefix[strcspn(prefix, " ")] = 0;
        snprintf(out, n, "%s/%s", prefix, name);
    } else {
        snprintf(out, n, "%s", name);
    }
}

int pc_tar_walk(const unsigned char *buf, long len, pc_mem_cb cb, void *ud)
{
    long off = 0;
    char *longname = NULL;        /* GNU 'L' / pax "path" */
    char *longlink = NULL;
    int rc = 0;

    while (off + 512 <= len) {
        const unsigned char *h = buf + off;
        if (h[0] == 0) {                       /* 两个空块 = 档尾 */
            const unsigned char *p = h;
            int allzero = 1;
            for (int i = 0; i < 512; i++)
                if (p[i]) {
                    allzero = 0;
                    break;
                }
            if (allzero)
                break;
        }
        char name[1024];
        tar_name(name, sizeof name, h);
        long size = tar_num(h + 124, 12);
        char type = (char)h[156];
        char link[1024];
        memcpy(link, h + 157, 100);
        link[100] = 0;
        link[strcspn(link, "\r\n")] = 0;
        mode_t mode = (mode_t)tar_num(h + 100, 8) & 07777;
        long dstart = off + 512;
        long databytes = size < 0 ? 0 : size;
        if (dstart + databytes > len)
            databytes = len - dstart;
        long next = dstart + ((size + 511) / 512) * 512;

        if (type == 'L') {                     /* GNU 长名 */
            free(longname);
            longname = malloc((size_t)databytes + 1);
            if (longname) {
                memcpy(longname, buf + dstart, (size_t)databytes);
                longname[databytes] = 0;
            }
            off = next;
            continue;
        }
        if (type == 'K') {                     /* GNU 长链接名 */
            free(longlink);
            longlink = malloc((size_t)databytes + 1);
            if (longlink) {
                memcpy(longlink, buf + dstart, (size_t)databytes);
                longlink[databytes] = 0;
            }
            off = next;
            continue;
        }
        if (type == 'x' || type == 'g') {      /* pax 扩展头 */
            if (type == 'x') {
                const unsigned char *p = buf + dstart;
                const unsigned char *e = buf + dstart + databytes;
                while (p < e) {
                    char *qq = NULL;
                    long reclen = strtol((const char *)p, &qq, 10);
                    const unsigned char *q = (const unsigned char *)qq;
                    if (!q || q >= e || reclen <= 0)
                        break;
                    const unsigned char *rec = q;
                    const unsigned char *rece = p + reclen;
                    if (rece > e)
                        rece = e;
                    const unsigned char *eq = memchr(rec, '=', (size_t)(rece - rec));
                    if (eq) {
                        long klen = (long)(eq - rec);
                        long vlen = (long)(rece - eq - 1);
                        /* 去掉尾部换行 */
                        while (vlen > 0 && (eq[1 + vlen - 1] == '\n'))
                            vlen--;
                        if (klen == 4 && !memcmp(rec, "path", 4)) {
                            free(longname);
                            longname = malloc((size_t)vlen + 1);
                            if (longname) {
                                memcpy(longname, eq + 1, (size_t)vlen);
                                longname[vlen] = 0;
                            }
                        } else if (klen == 8 && !memcmp(rec, "linkpath", 8)) {
                            free(longlink);
                            longlink = malloc((size_t)vlen + 1);
                            if (longlink) {
                                memcpy(longlink, eq + 1, (size_t)vlen);
                                longlink[vlen] = 0;
                            }
                        }
                    }
                    p += reclen;
                }
            }
            off = next;
            continue;
        }

        struct pc_mem m;
        memset(&m, 0, sizeof m);
        m.mode = mode;
        snprintf(m.name, sizeof m.name, "%s",
                 longname && longname[0] ? longname : name);
        free(longname);
        longname = NULL;
        if (type == '2') {
            m.type = PC_LNK;
            snprintf(m.link, sizeof m.link, "%s",
                     longlink && longlink[0] ? longlink : link);
        } else if (type == '1') {
            m.type = PC_HLNK;
            snprintf(m.link, sizeof m.link, "%s",
                     longlink && longlink[0] ? longlink : link);
        } else if (type == '5') {
            m.type = PC_DIR;
        } else if (type == '3') {
            m.type = PC_CHR;
        } else if (type == '4') {
            m.type = PC_BLK;
        } else if (type == '6') {
            m.type = PC_FIFO;
        } else if (type == '7') {
            m.type = PC_DIR;                   /* GNU dumpdir 当目录 */
        } else {
            m.type = PC_REG;                    /* '0' 与 NUL */
            m.data = buf + dstart;
            m.size = databytes;
        }
        if (type == '2' || type == '1')
            m.mode = 0777;
        free(longlink);
        longlink = NULL;
        rc = cb(&m, ud);
        if (rc)
            break;
        off = next;
    }
    free(longname);
    free(longlink);
    return rc;
}

/* ============================== cpio newc ============================== */

static unsigned cpio_hex(const unsigned char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char c = p[i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a') + 10u;
        else if (c >= 'A' && c <= 'F')
            d = (unsigned)(c - 'A') + 10u;
        else
            d = 0;
        v = v * 16u + d;
    }
    return v;
}

int pc_cpio_walk(const unsigned char *buf, long len, pc_mem_cb cb, void *ud)
{
    long off = 0;
    while (off + 110 <= len) {
        for (int i = 0; i < 6; i++)
            if (buf[off + i] != ((const unsigned char *)"070701")[i])
                return 0;                     /* 走到尾填充: 正常结束 */
        unsigned mode = cpio_hex(buf + off + 14);
        unsigned fsize = cpio_hex(buf + off + 54);
        unsigned nlink = cpio_hex(buf + off + 38);
        unsigned ino = cpio_hex(buf + off + 6);
        unsigned namesize = cpio_hex(buf + off + 94);
        off += 110;
        if (namesize == 0 || namesize > 4096 || off + namesize > len)
            return 0;
        char name[4096];
        unsigned nl = namesize < sizeof name ? namesize : (unsigned)sizeof name - 1;
        memcpy(name, buf + off, nl);
        name[nl] = 0;
        for (unsigned i = 0; i < nl; i++)
            if (name[i] == 0) {
                nl = i;
                break;
            }
        name[nl] = 0;
        off += (long)namesize;
        off = (off + 3) & ~3L;
        if (!strncmp(name, "TRAILER!!!", 10) && fsize == 0)
            return 0;
        if (off + fsize > len)
            return -1;
        /* cpio 成员名统一收成"相对名": "./usr/bin/x" -> "usr/bin/x",
         * "/usr/bin/x" -> "usr/bin/x"。原来那行 memmove 只删掉了 "./"
         * 本身, 把开头的 '/' 留在了串里("./usr" 从下标 1 搬 = "/usr"),
         * 于是 rpm -qpl 打出的路径全带一个多余的点。 */
        {
            char *nm2 = name;
            while (nm2[0] == '.' && nm2[1] == '/') {
                nm2 += 2;
                while (*nm2 == '/')
                    nm2++;
            }
            if (nm2 != name)
                memmove(name, nm2, strlen(nm2) + 1);
            else if (name[0] == '/')
                memmove(name, name + 1, strlen(name + 1) + 1);
        }

        struct pc_mem m;
        memset(&m, 0, sizeof m);
        if (strlen(name) >= sizeof m.name) {
            fprintf(stderr, "pkgcore: cpio 成员名过长(%zu 字节), 跳过\n",
                    strlen(name));
            return -1;
        }
        memcpy(m.name, name, strlen(name) + 1);
        m.mode = (mode_t)(mode & 07777);
        m.ino = ino;
        unsigned ft = mode & S_IFMT;
        if (ft == S_IFDIR) {
            m.type = PC_DIR;
            m.mode = m.mode ? m.mode : 0755;
        } else if (ft == S_IFLNK) {
            m.type = PC_LNK;
            long tn = fsize < (long)sizeof m.link - 1 ? fsize
                                                      : (long)sizeof m.link - 1;
            memcpy(m.link, buf + off, (size_t)tn);
            m.link[tn] = 0;
        } else if (ft == S_IFCHR) {
            m.type = PC_CHR;
        } else if (ft == S_IFBLK) {
            m.type = PC_BLK;
        } else if (ft == S_IFIFO) {
            m.type = PC_FIFO;
        } else if (ft == S_IFSOCK) {
            m.type = PC_SOCK;
        } else {
            /* 普通文件; 硬链接是 nlink>1 且 filesize==0 的那个成员 */
            if (nlink > 1 && fsize == 0) {
                m.type = PC_HLNK;
                m.mode = 0644;
            } else {
                m.type = PC_REG;
                m.data = buf + off;
                m.size = fsize;
                if (!m.mode)
                    m.mode = 0644;
            }
        }
        int rc = cb(&m, ud);
        off += ((fsize + 3) & ~3L);
        if (rc)
            return rc;
    }
    return 0;
}

/* ============================== 落盘 ============================== */

int pc_member_path(const char *root, const char *name, char *out, size_t n)
{
    if (!name || !name[0])
        return -1;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (*p < 0x20)
            return -1;                        /* 控制字符名: 拒绝 */

    /* 逐组件检查 ".."(任一组件等于 ".." 就逃逸出 root);
     * 绝对名与相对名一样落到 root 下 —— tar/cpio 都可能出现两种写法。 */
    const char *s = name;
    while (*s == '/')
        s++;
    for (const char *c = s;;) {
        const char *e = strchr(c, '/');
        size_t clen = e ? (size_t)(e - c) : strlen(c);
        if (clen == 2 && c[0] == '.' && c[1] == '.')
            return -1;
        if (!e)
            break;
        c = e + 1;
    }

    const char *rt = (root && *root && strcmp(root, "/")) ? root : "";
    /* 归一: 去掉开头的 "./" 与多余 '/'、去掉结尾的 '/', 使 "./bin/" 与
     * "/bin" 都落到 <root>/bin; 名字恰好是 "." 或空时结果就是 root 本身。 */
    char rel[1024];
    snprintf(rel, sizeof rel, "%s", s);
    char *rp = rel;
    while (*rp == '/')
        rp++;
    while (rp[0] == '.' && (rp[1] == '/' || rp[1] == 0)) {
        rp += 1;
        while (*rp == '/')
            rp++;
    }
    size_t rlen = strlen(rp);
    while (rlen > 0 && rp[rlen - 1] == '/')
        rp[--rlen] = 0;
    if (snprintf(out, n, "%s/%s", rt, rp) >= (int)n)
        return -1;
    /* 目录分隔折叠: "//" 在 POSIX 下等价 "/"，但为了让调用方比较 root 本身
     * (path == root) 好写, 这里把连续的 '/' 收成一个。 */
    {
        char *w = out, *r2 = out;
        int prev_slash = 0;
        for (; *r2; r2++) {
            if (*r2 == '/') {
                if (prev_slash)
                    continue;
                prev_slash = 1;
            } else {
                prev_slash = 0;
            }
            *w++ = *r2;
        }
        *w = 0;
        while (w > out && w[-1] == '/')                /* "root/." -> "root" */
            *--w = 0;
    }
    return 0;
}

int pc_mkdirs(const char *path, mode_t mode)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (buf[0]) {
                if (mkdir(buf, mode) < 0 && errno != EEXIST)
                    return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(buf, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int pc_write_file(const char *path, const unsigned char *data, long len,
                  mode_t mode)
{
    char parent[1024];
    snprintf(parent, sizeof parent, "%s", path);
    char *sl = strrchr(parent, '/');
    if (sl && sl != parent) {
        *sl = 0;
        if (pc_mkdirs(parent, 0755) < 0) {
            fprintf(stderr, "pkgcore: 建目录 %s 失败: %s\n",
                    parent, strerror(errno));
            return -1;
        }
    }
    /* 先 unlink 再建: 就地 O_TRUNC 一个正在运行的可执行文件会 ETXTBSY,
     * 且 O_CREAT 的 mode 只在创建时生效, 覆盖会留下旧权限。 */
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,
                  mode ? (mode & 07777) : 0644);
    if (fd < 0) {
        fprintf(stderr, "pkgcore: 打开 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    long done = 0;
    while (done < len) {
        ssize_t w = write(fd, data + done, (size_t)(len - done));
        if (w < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "pkgcore: 写 %s 失败: %s\n", path, strerror(errno));
            close(fd);
            return -1;
        }
        done += w;
    }
    if (close(fd) < 0) {
        fprintf(stderr, "pkgcore: 关闭 %s 失败: %s\n", path, strerror(errno));
        return -1;
    }
    if (chmod(path, mode ? (mode & 07777) : 0644) != 0 && errno != EROFS) {
        fprintf(stderr, "pkgcore: chmod %s %o 失败: %s\n",
                path, mode & 07777, strerror(errno));
        return -1;
    }
    return 0;
}

struct hnode {
    char *key;                                /* cpio: ino 字符串; tar: 成员名 */
    char *path;
    struct hnode *next;
};

static void hput(struct hnode **head, const char *key, const char *path)
{
    struct hnode *h = malloc(sizeof *h);
    if (!h)
        return;
    h->key = strdup(key);
    h->path = strdup(path);
    h->next = *head;
    *head = h;
}

static const char *hget(struct hnode *head, const char *key)
{
    for (struct hnode *h = head; h; h = h->next)
        if (!strcmp(h->key, key))
            return h->path;
    return NULL;
}

static void hfree(struct hnode *head)
{
    while (head) {
        struct hnode *n = head->next;
        free(head->key);
        free(head->path);
        free(head);
        head = n;
    }
}

struct pc_extract {
    const char *root;
    struct hnode *byino;
    struct hnode *byname;
    long nmem;
    int nerr;
    int quiet;
};

int pc_extract_init(struct pc_extract **out, const char *root)
{
    struct pc_extract *e = calloc(1, sizeof *e);
    if (!e)
        return -1;
    e->root = (root && *root) ? root : "";
    *out = e;
    return 0;
}

void pc_extract_free(struct pc_extract *e)
{
    if (!e)
        return;
    hfree(e->byino);
    hfree(e->byname);
    free(e);
}

long pc_extract_count(struct pc_extract *e)
{
    return e ? e->nmem : 0;
}

int pc_extract_errors(struct pc_extract *e)
{
    return e ? e->nerr : 0;
}

void pc_extract_quiet(struct pc_extract *e, int quiet)
{
    if (e)
        e->quiet = quiet;
}

int pc_extract_mem(struct pc_extract *e, const struct pc_mem *m)
{
    char path[1024];
    if (pc_member_path(e->root, m->name, path, sizeof path) != 0) {
        fprintf(stderr, "pkgcore: 拒绝不安全成员名 %s\n", m->name);
        e->nerr++;
        return -1;
    }
    /* 归档里 "." / "" 成员映射到 root 本身, 不需要落盘 */
    if (!strcmp(path, e->root) || !strcmp(path, ""))
        return 0;

    char key[32];
    switch (m->type) {
    case PC_DIR:
        if (pc_mkdirs(path, m->mode ? m->mode : 0755) < 0) {
            fprintf(stderr, "pkgcore: 建目录 %s 失败: %s\n",
                    path, strerror(errno));
            e->nerr++;
            return -1;
        }
        chmod(path, m->mode ? m->mode : 0755);
        break;
    case PC_LNK: {
        /* 只建**父**目录: 对 path 自己 mkdir 会把它变成目录, 后面 symlink
         * 就 EEXIST(实测: bin/pkgdemo-link 成了空目录, 软链没装上)。 */
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", path);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) {
            *sl = 0;
            pc_mkdirs(parent, 0755);
        }
        unlink(path);
        if (symlink(m->link, path) < 0) {
            fprintf(stderr, "pkgcore: 软链 %s -> %s 失败: %s\n",
                    path, m->link, strerror(errno));
            e->nerr++;
            return -1;
        }
        break;
    }
    case PC_HLNK: {
        const char *target = NULL;
        if (m->ino) {
            snprintf(key, sizeof key, "%lu", m->ino);
            target = hget(e->byino, key);
        }
        if (!target && m->link[0])
            target = hget(e->byname, m->link);
        if (!target) {
            if (!e->quiet)
                fprintf(stderr, "pkgcore: 硬链 %s 的原成员还没落盘, 跳过\n",
                        path);
            e->nerr++;
            return -1;
        }
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", path);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) {
            *sl = 0;
            pc_mkdirs(parent, 0755);
        }
        unlink(path);
        if (link(target, path) < 0) {
            fprintf(stderr, "pkgcore: 硬链 %s -> %s 失败: %s\n",
                    path, target, strerror(errno));
            e->nerr++;
            return -1;
        }
        break;
    }
    case PC_CHR:
    case PC_BLK:
    case PC_FIFO:
    case PC_SOCK: {
        char parent[1024];
        snprintf(parent, sizeof parent, "%s", path);
        char *sl = strrchr(parent, '/');
        if (sl && sl != parent) {
            *sl = 0;
            pc_mkdirs(parent, 0755);
        }
        unlink(path);
        mode_t st = (m->type == PC_CHR ? S_IFCHR : m->type == PC_BLK ? S_IFBLK
                  : m->type == PC_FIFO ? S_IFIFO : S_IFSOCK) | m->mode;
        if (mknod(path, st, 0) < 0) {
            if (!e->quiet)
                fprintf(stderr, "pkgcore: mknod %s 失败: %s(跳过)\n",
                        path, strerror(errno));
            /* 设备节点在容器/非 root 下常见失败, 不算包损坏 */
        }
        break;
    }
    default:
        if (pc_write_file(path, m->data, m->size, m->mode) < 0) {
            e->nerr++;
            return -1;
        }
        break;
    }
    if (m->ino) {
        snprintf(key, sizeof key, "%lu", m->ino);
        hput(&e->byino, key, path);
    }
    hput(&e->byname, m->name, path);
    e->nmem++;
    if (e->nmem % 200 == 0 && !e->quiet)
        fprintf(stderr, "pkgcore: 已落 %ld 个成员...(最新 %s)\n",
                e->nmem, path);
    return 0;
}

/* 归档遍历回调包装 */
static int extract_cb(const struct pc_mem *m, void *ud)
{
    return pc_extract_mem((struct pc_extract *)ud, m);
}

int pc_tar_extract(const unsigned char *buf, long len, const char *root,
                   long *nm)
{
    struct pc_extract *e;
    if (pc_extract_init(&e, root) < 0)
        return -1;
    int rc = pc_tar_walk(buf, len, extract_cb, e);
    if (nm)
        *nm = e->nmem;
    int err = e->nerr;
    pc_extract_free(e);
    return rc ? rc : (err ? -1 : 0);
}

int pc_cpio_extract(const unsigned char *buf, long len, const char *root,
                    long *nm)
{
    struct pc_extract *e;
    if (pc_extract_init(&e, root) < 0)
        return -1;
    int rc = pc_cpio_walk(buf, len, extract_cb, e);
    if (nm)
        *nm = e->nmem;
    int err = e->nerr;
    pc_extract_free(e);
    return rc ? rc : (err ? -1 : 0);
}

/* ============================== 版本比较 ============================== */

static int deb_order(int c)
{
    if (c == '~')
        return -1;
    if (c >= '0' && c <= '9')
        return c + 256;                        /* 数字一律排在其它字符之后 */
    return c;                                  /* '\0' -> 0, 排在 ~ 之后 */
}

static int deb_part_cmp(const char *a, const char *b)
{
    /* Debian Policy 5.6.12: 交替比较"非数字段"与"数字段"。
     * 逐字符跨段比较是错的(实测 1.9 与 1.10 会比成 1.9 > 1.10)。 */
    const char *sa = a, *sb = b;
    for (;;) {
        sa = a;
        sb = b;
        while (*sa && !isdigit((unsigned char)*sa))
            sa++;
        while (*sb && !isdigit((unsigned char)*sb))
            sb++;
        for (const char *pa = a, *pb = b; pa < sa || pb < sb; pa++, pb++) {
            int ca = deb_order(pa < sa ? (unsigned char)*pa : 0);
            int cb = deb_order(pb < sb ? (unsigned char)*pb : 0);
            if (ca != cb)
                return ca < cb ? -1 : 1;
        }
        a = sa;
        b = sb;
        while (*a == '0')
            a++;
        while (*b == '0')
            b++;
        const char *ea = a, *eb = b;
        while (isdigit((unsigned char)*ea))
            ea++;
        while (isdigit((unsigned char)*eb))
            eb++;
        long la = (long)(ea - a), lb = (long)(eb - b);
        if (la != lb)
            return la > lb ? 1 : -1;
        for (long i = 0; i < la; i++)
            if (a[i] != b[i])
                return (unsigned char)a[i] > (unsigned char)b[i] ? 1 : -1;
        a = ea;
        b = eb;
        if (!*a && !*b)
            return 0;
        /* 每轮至少吃掉一个字符(否则上一步的段比较就已经返回了), 不会死循环 */
    }
}

static long deb_epoch(const char *v, const char **rest)
{
    const char *c = strchr(v, ':');
    if (!c) {
        *rest = v;
        return 0;
    }
    long e = 0;
    for (const char *p = v; p < c && isdigit((unsigned char)*p); p++)
        e = e * 10 + (*p - '0');
    *rest = c + 1;
    return e;
}

int pc_deb_vercmp(const char *a, const char *b)
{
    const char *ra, *rb;
    long ea = deb_epoch(a ? a : "", &ra);
    long eb = deb_epoch(b ? b : "", &rb);
    if (ea != eb)
        return ea < eb ? -1 : 1;

    /* upstream = 最后一个 '-' 之前, revision = 之后 */
    const char *ha = strrchr(ra, '-');
    const char *hb = strrchr(rb, '-');
    char ua[512], ub[512], sa[512], sb[512];
    if (ha) {
        snprintf(ua, sizeof ua, "%.*s", (int)(ha - ra), ra);
        snprintf(sa, sizeof sa, "%s", ha + 1);
    } else {
        snprintf(ua, sizeof ua, "%s", ra);
        sa[0] = 0;
    }
    if (hb) {
        snprintf(ub, sizeof ub, "%.*s", (int)(hb - rb), rb);
        snprintf(sb, sizeof sb, "%s", hb + 1);
    } else {
        snprintf(ub, sizeof ub, "%s", rb);
        sb[0] = 0;
    }
    int r = deb_part_cmp(ua, ub);
    if (r)
        return r;
    return deb_part_cmp(sa, sb);
}

int pc_deb_ver_match(const char *have, const char *op, const char *want)
{
    if (!have || !want)
        return 0;
    int c = pc_deb_vercmp(have, want);
    if (!op || !*op)
        return c == 0;                          /* 无运算符 = 完全等于 */
    if (!strcmp(op, "<<") || !strcmp(op, "<"))    /* 单字符是 dpkg 的老写法 */
        return c < 0;
    if (!strcmp(op, ">>") || !strcmp(op, ">"))
        return c > 0;
    if (!strcmp(op, "<="))
        return c <= 0;
    if (!strcmp(op, ">="))
        return c >= 0;
    if (!strcmp(op, "="))
        return c == 0;
    fprintf(stderr, "pkgcore: 不认的版本关系运算符 '%s'\n", op);
    return 0;
}

static int rpm_segcmp(const char **pa, const char **pb)
{
    const char *a = *pa, *b = *pb;
    while (*a && !isalnum((unsigned char)*a))
        a++;
    while (*b && !isalnum((unsigned char)*b))
        b++;
    *pa = a;
    *pb = b;
    if (!*a || !*b)
        return -2;                              /* 有一边到头 */
    int da = isdigit((unsigned char)*a), db = isdigit((unsigned char)*b);
    if (da != db)
        return da ? -1 : 1;                     /* 数字段排在字母段之前 */
    if (da) {
        while (*a == '0')
            a++;
        while (*b == '0')
            b++;
        const char *ea = a, *eb = b;
        while (isdigit((unsigned char)*ea))
            ea++;
        while (isdigit((unsigned char)*eb))
            eb++;
        long la = (long)(ea - a), lb = (long)(eb - b);
        if (la != lb)
            return la > lb ? 1 : -1;
        while (a < ea) {
            if (*a != *b)
                return *a > *b ? 1 : -1;
            a++;
            b++;
        }
        *pa = a;
        *pb = b;
        return 0;
    }
    const char *sa = a, *sb = b;
    while (isalpha((unsigned char)*sa))
        sa++;
    while (isalpha((unsigned char)*sb))
        sb++;
    long la = (long)(sa - a), lb = (long)(sb - b);
    long n = la < lb ? la : lb;
    int r = memcmp(a, b, (size_t)n);
    if (r)
        return r > 0 ? 1 : -1;
    *pa = a + n;
    *pb = b + n;
    return 0;
}

static int rpm_verpart_cmp(const char *a, const char *b)
{
    for (;;) {
        int r = rpm_segcmp(&a, &b);
        if (r == -2)
            break;
        if (r)
            return r;
    }
    if (!*a && !*b)
        return 0;
    return *a ? 1 : -1;                         /* 剩内容的一方更大 */
}

/* ============================== SHA-256 ==============================
 * apt/yum 要核对索引里声明的校验和。这里**不**走 OpenSSL: 那两个管理器
 * 在没 TLS 的构建里也要能校验完整性, 而且实现小、可对着 sha256sum 逐字节验。
 */
static const unsigned K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

struct sha256 {
    unsigned h[8];
    unsigned char buf[64];
    unsigned long long total;
    size_t n;
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256 *c, const unsigned char *p)
{
    unsigned w[64], a, b, d, e, f, g, hh, t1, t2;
    unsigned cc;
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned)p[i * 4] << 24) | ((unsigned)p[i * 4 + 1] << 16) |
               ((unsigned)p[i * 4 + 2] << 8) | (unsigned)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        unsigned s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; hh = c->h[7];
    for (int i = 0; i < 64; i++) {
        unsigned S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        unsigned ch = (e & f) ^ ((~e) & g);
        t1 = hh + S1 + ch + K256[i] + w[i];
        unsigned S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        unsigned mj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += hh;
}

static void sha256_init(struct sha256 *c)
{
    static const unsigned iv0 = 0x6a09e667u, iv1 = 0xbb67ae85u,
                          iv2 = 0x3c6ef372u, iv3 = 0xa54ff53au,
                          iv4 = 0x510e527fu, iv5 = 0x9b05688cu,
                          iv6 = 0x1f83d9abu, iv7 = 0x5be0cd19u;
    c->h[0] = iv0; c->h[1] = iv1; c->h[2] = iv2; c->h[3] = iv3;
    c->h[4] = iv4; c->h[5] = iv5; c->h[6] = iv6; c->h[7] = iv7;
    c->n = 0;
    c->total = 0;
}

static void sha256_update(struct sha256 *c, const unsigned char *p, size_t len)
{
    c->total += len;
    while (len) {
        size_t take = 64 - c->n;
        if (take > len)
            take = len;
        memcpy(c->buf + c->n, p, take);
        c->n += take;
        p += take;
        len -= take;
        if (c->n == 64) {
            sha256_block(c, c->buf);
            c->n = 0;
        }
    }
}

static void sha256_final(struct sha256 *c, unsigned char out[32])
{
    unsigned long long bits = c->total * 8ULL;
    unsigned char pad = 0x80;
    sha256_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56)
        sha256_update(c, &pad, 1);
    unsigned char lb[8];
    for (int i = 0; i < 8; i++)
        lb[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

int pc_sha256_hex(const void *data, size_t len, char out[65])
{
    struct sha256 c;
    unsigned char d[32];
    sha256_init(&c);
    sha256_update(&c, (const unsigned char *)data, len);
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[64] = 0;
    return 0;
}

int pc_file_sha256_hex(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    struct sha256 c;
    sha256_init(&c);
    unsigned char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sha256_update(&c, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err)
        return -1;
    unsigned char d[32];
    sha256_final(&c, d);
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", d[i]);
    out[64] = 0;
    return 0;
}

int pc_rpm_vercmp(const char *a, const char *b)
{
    const char *ca = a ? a : "", *cb = b ? b : "";
    long ea = 0, eb = 0;
    const char *sa = strchr(ca, ':'), *sb = strchr(cb, ':');
    if (sa) {
        ea = strtol(ca, NULL, 10);
        ca = sa + 1;
    }
    if (sb) {
        eb = strtol(cb, NULL, 10);
        cb = sb + 1;
    }
    if (ea != eb)
        return ea < eb ? -1 : 1;
    return rpm_verpart_cmp(ca, cb);
}

/* RPM 风格的关系比较(yum 的 requires 里 flags=LT/GT/GE/LE/EQ 走这里)。
 * 接受上游常见的两种写法: "<" ">" "=" "!=" "<=" ">=" 与 "LT"/"GT"/... */
int pc_rpm_ver_match(const char *have, const char *op, const char *want)
{
    if (!have || !want)
        return 0;
    int c = pc_rpm_vercmp(have, want);
    if (!op || !*op)
        return c == 0;
    if (!strcmp(op, "<") || !strcmp(op, "LT") || !strcmp(op, "older"))
        return c < 0;
    if (!strcmp(op, ">") || !strcmp(op, "GT") || !strcmp(op, "newer"))
        return c > 0;
    if (!strcmp(op, "<=") || !strcmp(op, "LE"))
        return c <= 0;
    if (!strcmp(op, ">=") || !strcmp(op, "GE"))
        return c >= 0;
    if (!strcmp(op, "=") || !strcmp(op, "==") || !strcmp(op, "EQ") ||
        !strcmp(op, "equal"))
        return c == 0;
    if (!strcmp(op, "!="))
        return c != 0;
    fprintf(stderr, "pkgcore: 不认的 RPM 版本运算符 '%s'\n", op);
    return 0;
}
