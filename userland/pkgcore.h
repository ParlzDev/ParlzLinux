/* SPDX-License-Identifier: GPL-2.0-or-later */
/* pkgcore.h - dpkg/rpm/apt/yum 共用的归档与落盘原语。
 *
 * 四种包格式在容器层是同一批部件:
 *   .deb  = ar(debian-binary + control.tar + data.tar), tar 常 gzip 压缩
 *   .rpm  = lead + header + cpio 负载, 负载常 gzip 压缩
 *   索引  = Packages / primary.xml, 常 gzip 压缩
 * 所以 ar/tar/cpio 的读、gzip 解压、以及"安全落盘"(拒绝 .. 越界、先 unlink
 * 再建、显式 chmod)只写一遍, 四个管理器共用同一份实现 —— 也共用同一份坑。
 */
#ifndef PARLZ_PKGCORE_H
#define PARLZ_PKGCORE_H
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>

/* 归档成员类型(与 tar typeflag / cpio mode 对齐后的自有编码) */
#define PC_REG  'f'   /* 普通文件 */
#define PC_DIR  'd'   /* 目录 */
#define PC_LNK  'l'   /* 符号链接, link 字段 = 目标 */
#define PC_HLNK 'h'   /* 硬链接, link 字段 = 同归档内的另一个成员名 */
#define PC_CHR  'c'
#define PC_BLK  'b'
#define PC_FIFO 'p'
#define PC_SOCK 's'

struct pc_mem {
    char name[1024];       /* 归档内的相对名(已去掉 ./ 前缀) */
    char link[1024];       /* PC_LNK: 链接目标; PC_HLNK: 同包内另一成员名 */
    mode_t mode;           /* 权限位(不含类型位) */
    int type;              /* PC_* */
    const unsigned char *data;  /* 内容(PC_REG 才有意义) */
    long size;
    unsigned long ino;     /* cpio 用: 硬链接识别; tar 用: 0 */
};

/* 逐成员回调。返回非 0 让遍历提前终止(错误传递)。 */
typedef int (*pc_mem_cb)(const struct pc_mem *m, void *ud);

/* --- 解压 --- */
/* gzip(RFC1952)容器解压到堆。out 由调用方 free。
 * 返回 0 成功; -1 格式错/截断; -2 内存不足。 */
int pc_gunzip(const unsigned char *src, long srclen,
              unsigned char **out, long *outlen);
/* 按文件头魔数自动判断: gzip 就解压, 否则把原缓冲区交出去(不复制,
 * *owned 告知调用方是否需要 free —— 非 gzip 时指向 src, 不可 free)。 */
int pc_maybe_gunzip(const unsigned char *src, long srclen,
                    unsigned char **out, long *outlen, int *owned);

/* --- ar(GNU/SystemV 常规档, .deb 外层) --- */
/* 在 ar 档里按成员名取内容(名比较容忍前导 './'、'/' 与尾随 '/')。
 * 返回 0 命中(*data/*dlen 指向 src 内部), -1 档结构错, -2 未找到。 */
int pc_ar_member(const unsigned char *src, long srclen, const char *name,
                 const unsigned char **data, long *dlen);

/* --- tar(ustar + GNU 长名 + pax) --- */
int pc_tar_walk(const unsigned char *buf, long len, pc_mem_cb cb, void *ud);

/* --- cpio newc(070701) --- */
int pc_cpio_walk(const unsigned char *buf, long len, pc_mem_cb cb, void *ud);

/* --- 落盘 --- */
/* 把归档成员名安全地拼到 root 下: 拒绝 ".." 组件、控制字符、空名。
 * root 为 "" 或 "/" 时落真实根。返回 0 成功; -1 不安全(调用方须跳过并报错)。 */
int pc_member_path(const char *root, const char *name, char *out, size_t n);

/* 逐级建目录(已存在不算错)。 */
int pc_mkdirs(const char *path, mode_t mode);

/* 写普通文件: unlink-before-create(覆盖正在运行的可执行文件否则 ETXTBSY)、
 * 显式 chmod(O_CREAT 的 mode 只在创建时生效)、整值写完才认成功。 */
int pc_write_file(const char *path, const unsigned char *data, long len,
                  mode_t mode);

/* 逐成员落盘的抽取器: 内部维护 cpio 的 (ino -> 路径) 与 tar 的
 * (成员名 -> 路径) 两张表, 因此硬链接成员能正确 link 到同包内已落盘的原成员。 */
struct pc_extract;
int  pc_extract_init(struct pc_extract **out, const char *root);
void pc_extract_free(struct pc_extract *e);
int  pc_extract_mem(struct pc_extract *e, const struct pc_mem *m);
long pc_extract_count(struct pc_extract *e);   /* 已成功落盘的成员数 */
int  pc_extract_errors(struct pc_extract *e);  /* 失败成员数 */
void pc_extract_quiet(struct pc_extract *e, int quiet);

/* 一步把整个 tar/cpio 档抽到 root 下。返回 0 全成功; -1 有成员失败或档损坏。
 * *nm 返回落盘成员数。 */
int pc_tar_extract(const unsigned char *buf, long len, const char *root, long *nm);
int pc_cpio_extract(const unsigned char *buf, long len, const char *root, long *nm);

/* --- 版本比较(两套算法都返回 <0 / 0 / >0) --- */
int pc_deb_vercmp(const char *a, const char *b);
int pc_rpm_vercmp(const char *a, const char *b);
/* Debian 依赖串里的关系运算符: << >> <<= >>= = 与 NULL/""(等价 =) */
int pc_deb_ver_match(const char *have, const char *op, const char *want);
/* RPM/yum 的关系运算符: < > = != <= >= 与 LT GT EQ LE NE 字样 */
int pc_rpm_ver_match(const char *have, const char *op, const char *want);

/* 读整个文件到堆(返回 NULL 失败; *len 为长度)。 */
unsigned char *pc_slurp(const char *path, long *len);

/* SHA-256: 十六进制摘要写进 out(至少 65 字节)。apt/yum 用它核对索引里
 * 声明的校验和 —— 下载被截断或仓库里那份包坏了, 必须在这里挡住,
 * 不能交给 dpkg/rpm 去解一个内容不对的档。 */
int pc_sha256_hex(const void *data, size_t len, char out[65]);
int pc_file_sha256_hex(const char *path, char out[65]);

/* 路径的最后一段(不修改入参, 返回指向入参内部的指针)。 */
const char *pc_basename(const char *path);

#endif /* PARLZ_PKGCORE_H */
