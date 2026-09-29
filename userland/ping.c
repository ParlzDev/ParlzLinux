/* ping.c - 极简 ICMP echo 客户端(静态用户空间)。
 *
 * 用法: ping [-c <n>] [-w <sec>] <host|ip>
 *   -c <n>     发送 n 个包后停(默认无限, Ctrl+C 停)
 *   -w <sec>   总超时秒数(到时间自动停)
 *
 * 实现: SOCK_RAW + IPPROTO_ICMP(需 CAP_NET_RAW/root)。
 * 自实现 ICMP 头 + 校验和(不依赖 glibc icmp 结构),逐包
 * recvfrom + 按 icmp id/seq 过滤,打印往返 ms。
 * 包体 64B(总 84B),序列号从 1 起,id 用 getpid()&0xffff。
 * DNS 解析与 curl 同源: 优先 /etc/resolv.conf 的 nameserver 做
 * UDP A 查询(复用内联实现),失败则 gethostbyname 回退。
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <poll.h>

static int opt_count = 4;             /* -c: 默认 4 次, -c 0 = 无限 */
static int opt_wait = 30;            /* -w: 总超时秒数, 默认 30s */
static int sent_total = 0;
static int received_total = 0;
static int lost_total = 0;
static volatile sig_atomic_t g_stop = 0;

static void sigint_cb(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------- 校验和 ---------- */
static unsigned short icmp_checksum(const void *buf, int len)
{
    const unsigned char *p = buf;
    unsigned long sum = 0;
    int i;
    for (i = 0; i + 1 < len; i += 2)
        sum += (unsigned long)p[i] << 8 | (unsigned long)p[i + 1];
    if (i < len)
        sum += (unsigned long)p[i] << 8;
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return (unsigned short)~sum;
}

/* ---------- DNS: 自己发 UDP 查询,拿不到再退回 gethostbyname ----------
 *
 * resolv.conf 里可以有好几个 nameserver(QEMU 代理 + 公网兜底)。早先这里
 * 只取**第一个**,第一个不可达就一路失败到 gethostbyname —— 表现是
 * "ping 域名不通,但 curl 同一域名通"。现在逐个问,并且按 resolv.conf 的
 * `options timeout:N attempts:M` 定每服的等待,前面挂死的服务器只会
 * 让解析慢 N×M 秒,不会拖成假死。 */
static int read_resolv_ns(char ns[3][64], int max, int *timeout_ms,
                          int *attempts)
{
    FILE *f = fopen("/etc/resolv.conf", "r");
    char line[256];
    int found = 0;
    *timeout_ms = 2000;
    *attempts = 2;
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!strncmp(p, "nameserver", 10)) {
            p += 10;
            char *ip = p;
            while (*ip == ' ' || *ip == '\t')
                ip++;
            char *end = ip;
            while (*end && *end != '\n' && *end != '\r' &&
                   *end != ' ' && *end != '\t')
                end++;
            *end = 0;
            if (*ip && found < max) {
                snprintf(ns[found], 64, "%s", ip);
                found++;
            }
        } else if (!strncmp(p, "options", 7)) {
            char *tok = strtok(p + 7, " \t\r\n");
            while (tok) {
                if (!strncmp(tok, "timeout:", 8)) {
                    int v = atoi(tok + 8);
                    if (v >= 1 && v <= 30)
                        *timeout_ms = v * 1000;
                } else if (!strncmp(tok, "attempts:", 9)) {
                    int v = atoi(tok + 9);
                    if (v >= 1 && v <= 5)
                        *attempts = v;
                }
                tok = strtok(NULL, " \t\r\n");
            }
        }
    }
    fclose(f);
    return found;
}

/* 向单个 nameserver 问 host 的 A 记录。返回 1 成功。 */
static int dns_ask(const char *ns, const char *host, struct in_addr *out,
                   int timeout_ms, int attempts)
{
    struct sockaddr_in nsaddr;
    memset(&nsaddr, 0, sizeof nsaddr);
    nsaddr.sin_family = AF_INET;
    nsaddr.sin_port = htons(53);
    if (inet_pton(AF_INET, ns, &nsaddr.sin_addr) != 1)
        return 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return 0;
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    unsigned char q[512];
    memset(q, 0, sizeof q);
    q[0] = 0x12; q[1] = 0x34;
    q[2] = 0x01; q[3] = 0x00;
    q[4] = 0x00; q[5] = 0x01;
    int off = 12;
    const char *pp = host;
    while (*pp) {
        const char *dot = strchr(pp, '.');
        int len = dot ? (int)(dot - pp) : (int)strlen(pp);
        q[off++] = (unsigned char)len;
        memcpy(q + off, pp, len);
        off += len;
        if (!dot)
            break;
        pp = dot + 1;
    }
    q[off++] = 0x00;
    q[off++] = 0x00; q[off++] = 0x01;
    q[off++] = 0x00; q[off++] = 0x01;
    int qlen = off;
    sendto(s, q, qlen, 0, (struct sockaddr *)&nsaddr, sizeof nsaddr);
    int retries = attempts;
    unsigned char resp[2048];
    int ok = 0;
    for (;;) {
        struct pollfd pfd = { .fd = s, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) {
            if (--retries <= 0)
                break;
            sendto(s, q, qlen, 0,
                   (struct sockaddr *)&nsaddr, sizeof nsaddr);
            continue;
        }
        ssize_t n = recv(s, resp, sizeof resp, 0);
        if (n < 12)
            break;
        if (resp[0] != q[0] || resp[1] != q[1])
            break;
        int ancount = (resp[6] << 8) | resp[7];
        int m = 12;
        unsigned short qd = (resp[4] << 8) | resp[5];
        for (int i = 0; i < qd; i++) {
            while (resp[m])
                m += resp[m] + 1;
            m += 1 + 4;
        }
        for (int i = 0; i < ancount; i++) {
            unsigned char t = resp[m];
            if ((t & 0xC0) == 0xC0)
                m += 2;
            else {
                while (t) {
                    t = resp[m];
                    if ((t & 0xC0) == 0xC0) { m += 2; break; }
                    m += t + 1;
                }
                m++;
            }
            unsigned short type = (resp[m] << 8) | resp[m + 1];
            unsigned short cls = (resp[m + 2] << 8) | resp[m + 3];
            int rdlen = (resp[m + 8] << 8) | resp[m + 9];
            if (type == 1 && cls == 1 && rdlen == 4) {
                out->s_addr = ((uint32_t)resp[m + 10] << 24) |
                      ((uint32_t)resp[m + 11] << 16) |
                      ((uint32_t)resp[m + 12] << 8) |
                      ((uint32_t)resp[m + 13]);
                ok = 1;
                break;
            }
            m += 10 + rdlen;
        }
        break;
    }
    close(s);
    return ok;
}

/* 数字/主机名解析。返回 1 成功, 0 失败。 */
static int resolve_host(const char *host, struct in_addr *out)
{
    if (inet_pton(AF_INET, host, out) == 1)
        return 1;
    char ns[3][64];
    int tmo, att;
    int cnt = read_resolv_ns(ns, 3, &tmo, &att);
    for (int k = 0; k < cnt; k++) {
        if (dns_ask(ns[k], host, out, tmo, att))
            return 1;
    }
    struct hostent *he = gethostbyname(host);
    if (he && he->h_addr_list[0]) {
        *out = *(struct in_addr *)he->h_addr_list[0];
        return 1;
    }
    return 0;
}

/* ---------- ICMP 包收发 ---------- */
#define PING_PAYLOAD 64

static int send_ping(int s, const struct in_addr *dst, int seq,
                     struct timeval *ts)
{
    unsigned char pkt[8 + PING_PAYLOAD + 8];
    struct icmphdr *icp = (struct icmphdr *)pkt;
    int plen = 8 + PING_PAYLOAD;
    memset(pkt, 0, plen);
    icp->type = ICMP_ECHO;
    icp->code = 0;
    icp->un.echo.id = (uint16_t)(getpid() & 0xffff);
    icp->un.echo.sequence = htons(seq);
    /* 载荷: 前 8 字节 = 发送时间戳(秒+微秒),后续填 0 */
    memcpy(pkt + 8, ts, sizeof *ts);
    for (int i = 16; i < plen; i++)
        pkt[i] = (unsigned char)(i & 0xff);
    icp->checksum = 0;
    icp->checksum = icmp_checksum(pkt, plen);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr = *dst;
    ssize_t r = sendto(s, pkt, plen, 0,
                       (struct sockaddr *)&sa, sizeof sa);
    return (r == plen) ? 0 : -1;
}

/* 收一个匹配 seq 的 ICMP echo 回复, 1s 超时。返回 0 成功(填 rt_us),
 * -1 超时(填 -1)。 */
static int recv_ping(int s, const struct in_addr *dst, int seq,
                     long *rt_us)
{
    unsigned char buf[1024];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof from;
    struct timeval tv = { 1, 0 };
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);
    int pr = select(s + 1, &rfds, NULL, NULL, &tv);
    if (pr <= 0) {
        *rt_us = -1;
        return -1;
    }
    ssize_t n = recvfrom(s, buf, sizeof buf, 0,
                         (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        *rt_us = -1;
        return -1;
    }
    /* IP 头(20B) + ICMP 头(8B) */
    if (n < 20 + 8) {
        *rt_us = -1;
        return -1;
    }
    struct icmphdr *icp = (struct icmphdr *)(buf + 20);
    if (icp->type != ICMP_ECHOREPLY) {
        *rt_us = -1;
        return -1;
    }
    if (icp->un.echo.id != (uint16_t)(getpid() & 0xffff)) {
        *rt_us = -1;
        return -1;
    }
    if (ntohs(icp->un.echo.sequence) != seq) {
        *rt_us = -1;
        return -1;
    }
    /* 取发送时间戳(包偏移 20 + 8 = 28) */
    struct timeval ts;
    memcpy(&ts, buf + 20 + 8, sizeof ts);
    struct timeval now;
    gettimeofday(&now, NULL);
    long dt_us = (long)(now.tv_sec - ts.tv_sec) * 1000000L +
                 (now.tv_usec - ts.tv_usec);
    if (dt_us < 0)
        dt_us = 0;
    *rt_us = dt_us;
    (void)dst;
    return 0;
}

int main(int argc, char *argv[])
{
    int i = 1;
    const char *host = NULL;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc)
            opt_count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w") && i + 1 < argc)
            opt_wait = atoi(argv[++i]);
        else if (argv[i][0] != '-')
            host = argv[i];
    }
    if (!host) {
        fprintf(stderr,
                "ping (Parlz ICMP echo)\n"
                "用法: ping [-c <n>] [-w <sec>] <host|ip>\n");
        return 1;
    }
    signal(SIGINT, sigint_cb);

    struct in_addr dst;
    if (!resolve_host(host, &dst)) {
        fprintf(stderr, "ping: cannot resolve %s\n", host);
        return 1;
    }
    char ipstr[64];
    inet_ntop(AF_INET, &dst, ipstr, sizeof ipstr);

    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) {
        fprintf(stderr,
                "ping: open raw socket failed (%s); "
                "需 root + 内核 CONFIG_RAW_DIAG/ICMP 支持\n",
                strerror(errno));
        return 1;
    }

    fprintf(stderr, "PING %s (%s)\n", host, ipstr);
    fflush(stderr);

    time_t t0 = time(NULL);
    int seq = 1;
    long total_rt_us = 0;
    int min_rt_ms = 0, max_rt_ms = 0, have_rt = 0;

    for (;;) {
        if (g_stop)
            break;
        if (opt_count > 0 && sent_total >= opt_count)
            break;
        if (opt_wait > 0 && time(NULL) - t0 >= opt_wait)
            break;

        struct timeval ts;
        gettimeofday(&ts, NULL);
        sent_total++;
        if (send_ping(s, &dst, seq, &ts) < 0) {
            fprintf(stderr, "ping: send failed: %s\n", strerror(errno));
            lost_total++;
            seq++;
            if (opt_count > 0 && sent_total >= opt_count)
                break;
            continue;
        }
        fflush(stderr);
        long rt_us;
        int ok = recv_ping(s, &dst, seq, &rt_us);
        if (ok == 0) {
            received_total++;
            int rt_ms = (int)(rt_us / 1000);
            total_rt_us += rt_us;
            if (!have_rt || rt_ms < min_rt_ms) min_rt_ms = rt_ms;
            if (!have_rt || rt_ms > max_rt_ms) max_rt_ms = rt_ms;
            have_rt = 1;
            printf("%d bytes from %s: icmp_seq=%d ttl=64 time=%d ms\n",
                   64 + 8, ipstr, seq, rt_ms);
        } else {
            lost_total++;
            printf("Request timeout for icmp_seq=%d\n", seq);
        }
        fflush(stdout);
        seq++;
        if (seq == 0)
            seq = 1;
        sleep(1);
    }

    /* 汇总行 */
    if (sent_total > 0)
        printf("--- %s ping statistics ---\n", host);
    printf("%d packets transmitted, %d received, %d lost\n",
           sent_total, received_total, lost_total);
    if (received_total > 0 && have_rt) {
        double avg_ms = (double)total_rt_us / received_total / 1000.0;
        printf("rtt min/avg/max = %d/%.3f/%d ms\n",
               min_rt_ms, avg_ms, max_rt_ms);
    }
    close(s);
    return (lost_total > 0) ? 1 : 0;
}
