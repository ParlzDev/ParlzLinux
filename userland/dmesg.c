/* dmesg.c - 打印内核日志 ring buffer(保留最后 N 行,默认 200)。
 *
 * 读取策略:
 *   1) 优先 /dev/kmsg:read(2) 驱动,每次 read 返回一条完整记录,
 *      格式 "<PRI>[<PRTIME>]<SEQ> <MSG>\n"。非阻塞读直到 0/-EAGAIN,
 *      保留最后 N 行输出。
 *   2) 回退 /proc/kmsg:同样非阻塞逐行读(与 klogctl 等价)。
 *
 * 行缓存:固定 MAX_LINES 行。
 * 用法: dmesg [-n <lines>]   无参数默认 200 行; -n 0 = 全部。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_LINES 4096

static char *lines[MAX_LINES];
static int nlines = 0;

static void add_line(const char *s, int len)
{
    if (nlines >= MAX_LINES)
        return;
    char *c = malloc((size_t)len + 1);
    if (!c)
        return;
    memcpy(c, s, (size_t)len);
    c[len] = 0;
    lines[nlines++] = c;
}

/* 解析 /dev/kmsg 记录:
 * 格式 "<PRI>[<PRTIME>]<SEQ> <MSG>"(例 "1[    2.819589]   42: msg")。
 * 目标是剥掉 "<PRI>[<PRTIME>]<SEQ>" 前缀,保留 "<PRTIME> <MSG>" 观感。
 * 实现:找形如 " <纯数字>: " 的序列边界(数字后紧跟 ": "),其后为消息体;
 * 找到后回退到该数字前的 PRTIME 段(形如 "   12.345678" 或 " [ 1.2]"),
 * 输出 "<PRTIME> <MSG>"。找不到则原样输出整行。 */
static void parse_kmsg_record(const char *s, int len)
{
    const char *sep = NULL;
    for (int i = 0; i < len; i++) {
        if (s[i] != ' ' || i + 1 >= len)
            continue;
        int j = i + 1;
        if (!isdigit((unsigned char)s[j]))
            continue;
        while (j < len && isdigit((unsigned char)s[j]))
            j++;
        if (j < len && s[j] == ':') {
            sep = s + i;
            break;
        }
    }
    if (!sep) {
        add_line(s, len);
        return;
    }
    /* 回退到 PRTIME:从 sep 往前找 PRTIME 起点。
     * /dev/kmsg 记录 PRTIME 形如 "    2.819589"(前导空格 + 数字 + 点),
     * 再往前是 '['(PRTIME 段以 [ 开头)或记录以 PRI 数字开头。
     * 简化:找 '[' 后的 PRTIME;若无 '[' 则直接以 sep 为消息体起点。 */
    const char *body = sep;
    const char *bracket = memchr(s, '[', (size_t)(sep - s));
    if (bracket) {
        /* PRTIME 是 bracket+1 起的内容(数字 + 点);body 从 PRTIME 后取 */
        body = bracket + 1;
        /* 跳过 PRTIME 数字 + 点 + 右括号(若存在) */
        const char *p = body;
        while (*p && (*p >= '0' && *p <= '9' || *p == '.' || *p == ':'))
            p++;
        if (*p == ']')
            p++;
        /* 跳过一个空格 */
        if (*p == ' ')
            p++;
        /* PRTIME 段(从 bracket+1 到 p)+ 消息体(p 起) */
        int prlen = (int)(p - body);
        int bodylen = len - (int)(p - s);
        int total = prlen + 1 + bodylen;
        char *out = malloc((size_t)total + 1);
        if (out) {
            int w = 0;
            memcpy(out + w, body, (size_t)prlen); w += prlen;
            out[w++] = ' ';
            memcpy(out + w, p, (size_t)bodylen); w += bodylen;
            out[w] = 0;
            add_line(out, w);
        } else {
            add_line(s, len);
        }
    } else {
        add_line(sep, len - (int)(sep - s));
    }
}

static void flush(int keep)
{
    int start = nlines > keep ? nlines - keep : 0;
    for (int i = start; i < nlines; i++)
        puts(lines[i]);
}

/* 读 /dev/kmsg 全部记录,保留最后 keep 行 */
static int try_dev_kmsg(int keep)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        /* 可能一次多条记录,逐条以 \n 切 */
        int off = 0;
        while (off < n) {
            int len = 0;
            while (off + len < n && buf[off + len] != '\n')
                len++;
            if (len > 0)
                parse_kmsg_record(buf + off, len);
            off += len + 1;
        }
    }
    close(fd);
    flush(keep);
    return 0;
}

/* 回退:/proc/kmsg 非阻塞逐行读 */
static int try_proc_kmsg(int keep)
{
    int fd = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break;
        int off = 0;
        while (off < n) {
            int len = 0;
            while (off + len < n && buf[off + len] != '\n')
                len++;
            if (len > 0)
                parse_kmsg_record(buf + off, len);
            off += len + 1;
        }
    }
    close(fd);
    flush(keep);
    return 0;
}

int main(int argc, char *argv[])
{
    int keep = 200;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) {
            keep = atoi(argv[++i]);
            if (keep < 0)
                keep = 0;
        }
    }
    if (try_dev_kmsg(keep) == 0 && nlines > 0)
        return 0;
    if (try_proc_kmsg(keep) == 0 && nlines > 0)
        return 0;
    fprintf(stderr, "dmesg: 无法读取内核日志(/dev/kmsg 与 /proc/kmsg 均不可用)\n");
    return 1;
}
