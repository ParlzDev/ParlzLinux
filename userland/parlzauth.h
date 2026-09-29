/* SPDX-License-Identifier: GPL-2.0 */
/* parlzauth.h - /etc/parlz-auth 的唯一读写实现(账户表 + 口令散列)。
 *
 * login(认证)与 user(账户管理)共用这一份 —— 文件格式、散列参数、明文兼容
 * 规则都只在这里写一次,免得两个命令各写一套慢慢走偏。
 *
 * 文件格式(新):
 *   # 注释行
 *   用户名:$6$…      每行一个账户
 * 旧格式(已分发出去的盘上是这样,读得到、登得进,登录成功时迁到新格式):
 *   第 1 行 = 用户名;第 2 行 = 口令($6$ 散列或明文)。
 */
#ifndef PARLZAUTH_H
#define PARLZAUTH_H

#include <stddef.h>

#define PA_AUTH_PATH  "/etc/parlz-auth"
#define PA_MAX_USERS  32
#define PA_NAME_MAX   32
#define PA_SECRET_MAX 128
#define PA_DIGEST_LEN 128     /* "$6$"+16 盐+'$'+86 摘要 = 106,留余量 */

struct pa_user {
    char name[PA_NAME_MAX];
    char secret[PA_SECRET_MAX];
};

/* 读全部账户。返回账户数;0 = 没有凭证(该走首次设置);-1 = 读失败。
 * was_legacy 非空时回填"文件是旧两行格式"(调用方据此决定是否迁移)。 */
int pa_load(struct pa_user *out, int max, int *was_legacy);

/* 整表落盘(0600, root 属主)。0 成功 */
int pa_save(const struct pa_user *u, int n);

/* 新口令 -> $6$(SHA-512 crypt,自带新盐)散列。0 成功 */
int pa_hash(const char *pass, char *digest, size_t n);

/* 校验口令:secret 是 $6$ 散列就按散列比,旧明文按明文比。1 = 通过 */
int pa_verify(const char *pass, const char *secret);

/* secret 是不是旧明文(不以 '$' 开头) */
int pa_is_plain(const char *secret);

/* 按名字查下标,找不到 -1 */
int pa_find(const struct pa_user *u, int n, const char *name);

/* 用户名合法性:非空、不以 '#' 开头、无 ':'/空白/控制字符、长度 < PA_NAME_MAX */
int pa_name_ok(const char *name);

/* 口令输入:操作 tty(串口/屏幕)时关回显逐字符读、用 '*' 占位;非 tty 读行。
 * 返回 0 成功 / 1 读到空 / -1 读失败(EOF)。 */
int pa_read_pass(char *buf, size_t n);

/* 读一行(去尾 \n)。0 成功 / -1 EOF */
int pa_read_line(char *buf, size_t n);

/* 把"本次认证通过的用户名"写到运行期文件,给启动 body 导出 $USER 用
 * (login 是子进程,setenv 传不回父进程)。0 成功 */
#define PA_WHOAMI_PATH "/tmp/.parlz-login-user"
int pa_write_whoami(const char *name);

#endif /* PARLZAUTH_H */
