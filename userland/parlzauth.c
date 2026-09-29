/* SPDX-License-Identifier: GPL-2.0 */
/* parlzauth.c - /etc/parlz-auth 的读写与口令散列(见 parlzauth.h)。 */
#include "parlzauth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <crypt.h>
#include <sys/stat.h>

/* ---------- 口令散列: libxcrypt 的 SHA-512 crypt($6$),静态链 -lcrypt ----------
 * 为什么不留明文: 装好的整盘 IMG 是挂在官网上当下载物的,明文密码会跟着
 * 盘一起分发 —— 0600 挡不住"拿到盘文件"的人。
 * crypt_r 的 struct crypt_data 有 8 KB 缓冲,一次性栈上分配即可。 */
static void make_salt(char *out, size_t n)
{
    static const char al[] =
        "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char rnd[16];
    int fd = open("/dev/urandom", O_RDONLY);
    int got = (fd >= 0) ? (int)read(fd, rnd, sizeof rnd) : -1;
    if (fd >= 0)
        close(fd);
    if (got != (int)sizeof rnd) {
        /* 取不到 urandom 也得有盐(退化到 time^pid,至少不撞同盐) */
        unsigned long s = (unsigned long)time(NULL) ^
                          ((unsigned long)getpid() << 11);
        for (size_t i = 0; i < sizeof rnd; i++) {
            s = s * 6364136223846793005UL + 1442695040888963407UL;
            rnd[i] = (unsigned char)(s >> 33);
        }
    }
    size_t p = (size_t)snprintf(out, n, "$6$");
    for (size_t i = 0; i < sizeof rnd && p + 1 < n; i++)
        out[p++] = al[rnd[i] % (sizeof al - 1)];
    out[p] = 0;
}

/* pw 按 setting 散列进 out;0 成功。libxcrypt 失败时回 "*0"/"*1"。 */
static int pw_crypt(const char *pw, const char *setting, char *out, size_t n)
{
    struct crypt_data cd;
    memset(&cd, 0, sizeof cd);
    char *h = crypt_r(pw, setting, &cd);
    if (!h || h[0] != '$')
        return -1;
    if (strlen(h) >= n)
        return -1;
    strcpy(out, h);
    return 0;
}

int pa_hash(const char *pass, char *digest, size_t n)
{
    char salt[64];
    make_salt(salt, sizeof salt);
    return pw_crypt(pass, salt, digest, n);
}

int pa_is_plain(const char *secret)
{
    return !secret || secret[0] != '$';
}

int pa_verify(const char *pass, const char *secret)
{
    if (!pass || !secret || !*secret)
        return 0;
    if (pa_is_plain(secret))
        return strcmp(pass, secret) == 0;
    char got[PA_DIGEST_LEN];
    return pw_crypt(pass, secret, got, sizeof got) == 0 && !strcmp(got, secret);
}

int pa_name_ok(const char *name)
{
    if (!name || !*name || name[0] == '#')
        return 0;
    size_t l = strlen(name);
    if (l >= PA_NAME_MAX)
        return 0;
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == ':' || c <= 0x20 || c == 0x7f)
            return 0;
    }
    return 1;
}

int pa_find(const struct pa_user *u, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(u[i].name, name))
            return i;
    return -1;
}

/* 读整个文件(受 PA_MAX_USERS×行长 限制就够) */
static ssize_t read_all(char *buf, size_t cap)
{
    int fd = open(PA_AUTH_PATH, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t total = 0;
    ssize_t r;
    while ((r = read(fd, buf + total, cap - 1 - total)) > 0) {
        total += (size_t)r;
        if (total >= cap - 1)
            break;
    }
    close(fd);
    buf[total] = 0;
    return (ssize_t)total;
}

/* 去掉尾部 \r\n */
static void rstrip(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r'))
        s[--l] = 0;
}

int pa_load(struct pa_user *out, int max, int *was_legacy)
{
    char buf[PA_MAX_USERS * (PA_NAME_MAX + PA_SECRET_MAX) + 256];
    if (was_legacy)
        *was_legacy = 0;
    ssize_t total = read_all(buf, sizeof buf);
    if (total <= 0)
        return 0;               /* 文件不存在/空 -> 首次设置 */

    /* 跳过空行与 # 注释, 找第一条内容行判断格式 */
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    char *first = NULL;
    while (line) {
        rstrip(line);
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p && *p != '#') {
            first = p;
            break;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    if (!first)
        return 0;

    if (!strchr(first, ':')) {
        /* 旧两行格式: 第 1 行用户名, 第 2 行口令(可能明文) */
        if (was_legacy)
            *was_legacy = 1;
        if (max < 1)
            return 0;
        snprintf(out[0].name, PA_NAME_MAX, "%s", first);
        char *second = strtok_r(NULL, "\n", &save);
        if (second) {
            rstrip(second);
            snprintf(out[0].secret, PA_SECRET_MAX, "%s", second);
        }
        return *out[0].name ? 1 : 0;
    }

    /* 新格式: 每行 "用户名:口令" */
    int n = 0;
    for (char *l = first; l && n < max; l = strtok_r(NULL, "\n", &save)) {
        rstrip(l);
        char *p = l;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#')
            continue;
        char *colon = strchr(p, ':');
        if (!colon)
            continue;           /* 没有 ':' 的行当噪声跳过 */
        *colon = 0;
        snprintf(out[n].name, PA_NAME_MAX, "%s", p);
        snprintf(out[n].secret, PA_SECRET_MAX, "%s", colon + 1);
        if (out[n].name[0])
            n++;
    }
    return n;
}

int pa_save(const struct pa_user *u, int n)
{
    char buf[PA_MAX_USERS * (PA_NAME_MAX + PA_SECRET_MAX) + 256];
    size_t off = (size_t)snprintf(buf, sizeof buf,
        "# ParlzOS 账户表 —— 每行一个 \"用户名:口令散列($6$)\"\n"
        "# 由 /bin/login(首次设置、旧格式迁移)与 /bin/user(add/rm/upd)维护\n");
    for (int i = 0; i < n && off + PA_NAME_MAX + PA_SECRET_MAX + 4 < sizeof buf; i++) {
        off += (size_t)snprintf(buf + off, sizeof buf - off, "%s:%s\n",
                                u[i].name, u[i].secret);
    }

    /* 先写临时文件再 rename: 中途掉电不会把凭证文件写空(那时谁都登不进去)。
     * fsync + 收尾 sync 是给"设完密码立刻断电/被 kill"的场景兜底 ——
     * 不给的话那次写入只在页缓存里, 断电就没了(实测: 验收脚本 kill QEMU 之后
     * 宿主挂盘复核, 文件根本不在; 用户侧则是"设了密码, 下次启动又要重设")。 */
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%s.new", PA_AUTH_PATH);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "parlzauth: 无法写入 %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    ssize_t w = write(fd, buf, off);
    if (w != (ssize_t)off || fchmod(fd, 0600) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, PA_AUTH_PATH) < 0) {
        fprintf(stderr, "parlzauth: rename %s 失败: %s\n", tmp,
                strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (chown(PA_AUTH_PATH, 0, 0) < 0 && errno != EPERM)
        fprintf(stderr, "parlzauth: chown 失败: %s\n", strerror(errno));
    sync();                     /* 目录项也落盘(rename 本身没有 fsync 语义) */
    return 0;
}

int pa_read_line(char *buf, size_t n)
{
    buf[0] = 0;
    if (!fgets(buf, (int)n, stdin))
        return -1;
    size_t l = strlen(buf);
    int saw_nl = (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'));
    if (saw_nl) {
        while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r'))
            buf[--l] = 0;
        return 0;
    }
    /* 行比缓冲长: 把这一行**剩下的字符吃掉**。不吃的后果很具体 ——
     * 多出来的字符会留在输入队列里, 被紧接着的 pa_read_pass 当口令读走,
     * 用户看到的是"用户名打长了半截, 密码就永远不对"。 */
    int c;
    while ((c = getchar()) != EOF && c != '\n' && c != '\r')
        ;
    return 0;
}

/* 读密码: tty 时关回显+关 ICANON 逐字符读, 每个可打印字符回显 '*';
 * 非 tty 读行(自动化)。返回 0 成功 / 1 空 / -1 EOF。 */
int pa_read_pass(char *buf, size_t n)
{
    buf[0] = 0;
    struct termios old, raw;
    int tty_ok = isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old) == 0;
    if (!tty_ok)
        return pa_read_line(buf, n) < 0 ? -1 : (buf[0] ? 0 : 1);
    raw = old;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    size_t len = 0;
    int failed = 0;
    for (;;) {
        unsigned char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) {
            failed = 1;
            break;
        }
        if (c == '\n' || c == '\r')   /* raw 下回车/换行都是字面字节 */
            break;
        if (c == 0x7f || c == 0x08)   /* 退格: 简化——回显的 '*' 不回退 */
            continue;
        if (c >= 0x20 && len < n - 1) {
            buf[len++] = (char)c;
            fputc('*', stdout);
            fflush(stdout);
        }
    }
    buf[len] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
    if (failed)
        return -1;
    return len == 0 ? 1 : 0;
}

int pa_write_whoami(const char *name)
{
    FILE *f = fopen(PA_WHOAMI_PATH, "w");
    if (!f)
        return -1;
    fprintf(f, "%s\n", name);
    fclose(f);
    chmod(PA_WHOAMI_PATH, 0600);
    return 0;
}
