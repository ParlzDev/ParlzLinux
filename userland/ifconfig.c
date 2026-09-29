/* ifconfig.c - 查看网络接口。用法: ifconfig [ifname]
 * 读 /sys/class/net/<if>/ 与 /sys/class/net/<if>/address;
 * 无参数时列全部非 lo 接口。无 netlink 依赖(纯 sysfs)。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

/* RTM_GETADDR 查询接口 IP。
 * 注意:RTM_GETADDR 响应分多批(每批 4K),单次 read 读不全;
 * 循环读到 NLMSG_DONE 或 NLMSG_ERROR(err=0)为止,带 2s 读超时
 * 防 netlink 数据面异常永久阻塞挂死 shell。 */
static void dump_addr(const char *ifname, int ifidx)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return;
    struct sockaddr_nl snl;
    memset(&snl, 0, sizeof snl);
    snl.nl_family = AF_NETLINK;
    /* dump 套接字必须 bind() 而不是 connect():
     * 内核 __netlink_dump_start() 按 portid 找 socket,而 portid 由
     * 第一次 sendmsg 时 netlink_autobind() 自动分配。connect() 会把
     * autobind 分配的 portid 覆盖成内核 portid(0),done/error 消息
     * 找不到接收者 -> 内核回 -EHOSTUNREACH(用户侧表现为失败)。
     * bind(portid=0) 不动 portid,autobind 在首包时正常分配。 */
    if (bind(fd, (struct sockaddr *)&snl, sizeof snl) < 0) {
        close(fd);
        return;
    }
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct {
        struct nlmsghdr nh;
        struct ifaddrmsg ifa;
    } req;
    memset(&req, 0, sizeof req);
    req.nh.nlmsg_len = NLMSG_HDRLEN + sizeof(struct ifaddrmsg);
    req.nh.nlmsg_type = RTM_GETADDR;
    /* 纯 dump 请求:不带 NLM_F_ACK。内核 dumper 完成时发 NLMSG_DONE,
     * 请求方收不到 ACK(ACK 只针对 doit 请求)。加 NLM_F_ACK 会让
     * 内核对 dump 报 EOPNOTSUPP("Operation not supported") */
    req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nh.nlmsg_seq = 1;
    req.ifa.ifa_family = AF_INET;
    /* 过滤指定接口的地址;ifa_index=0 表示全部接口。
     * 严格校验模式下头部 prefixlen/flags/scope 必须为 0,memset 后满足 */
    req.ifa.ifa_index = (unsigned int)ifidx;
    if (write(fd, &req, req.nh.nlmsg_len) < 0) {
        close(fd);
        return;
    }
    char buf[8192];
    int rounds = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0)
            break;               /* 超时/出错:已读到的部分够用 */
        rounds++;
        if (rounds > 50)        /* 死循环保护 */
            break;
        int done = 0;
        for (struct nlmsghdr *m = (struct nlmsghdr *)buf;
             NLMSG_OK(m, (size_t)n); m = NLMSG_NEXT(m, n)) {
            if (m->nlmsg_type == NLMSG_DONE) {
                /* dump 结束标记:后续可能还有 NEWADDR 批次,
                 * 继续读下一批直到超时为止 */
                continue;
            }
            if (m->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(m);
                if (e->error != 0) {
                    fprintf(stderr, "ifconfig: netlink 查 IP 失败: %s\n",
                            strerror(-e->error));
                    done = 1;
                }
                break;
            }
            if (m->nlmsg_type != RTM_NEWADDR)
                continue;
            struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(m);
            if (ifa->ifa_index != ifidx)
                continue;
            int alen = m->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));
            for (struct rtattr *a =
                     (struct rtattr *)((char *)ifa +
                                       NLMSG_ALIGN(sizeof(*ifa)));
                 RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                if (a->rta_type == IFA_LOCAL && a->rta_len >= 8) {
                    char ip[32];
                    struct in_addr in;
                    memcpy(&in, RTA_DATA(a), sizeof in);
                    inet_ntop(AF_INET, &in, ip, sizeof ip);
                    printf(" inet %s/%d\n", ip, ifa->ifa_prefixlen);
                }
            }
        }
        if (done)
            break;
        /* 本批读完且无错误:等下一批;2s 超时后 read 返回 EAGAIN,
         * 说明 dump 已结束,退出 */
    }
    close(fd);
}

/* 读 /sys/class/net/<if>/operstate + /sys/class/net/<if>/flags 判断状态 */
static void show_status(const char *ifname)
{
    char p[128];
    char buf[32];
    unsigned long flags = 0;
    int oper = 0;
    snprintf(p, sizeof p, "/sys/class/net/%s/flags", ifname);
    FILE *f = fopen(p, "r");
    if (f) {
        if (fscanf(f, "%lu", &flags) != 1)
            flags = 0;
        fclose(f);
    }
    snprintf(p, sizeof p, "/sys/class/net/%s/operstate", ifname);
    f = fopen(p, "r");
    if (f) {
        if (fgets(buf, sizeof buf, f)) {
            buf[strcspn(buf, "\n")] = 0;
            if (!strcmp(buf, "up"))
                oper = 1;
        }
        fclose(f);
    }
    /* IFF_UP=0x1, IFF_BROADCAST=0x2, IFF_RUNNING=0x40, IFF_MULTICAST=0x10000 */
    printf("%s%s%s%s",
           (oper || (flags & 0x1)) ? "UP" : "DOWN",
           (flags & 0x2) ? " BROADCAST" : "",
           (flags & 0x40) ? " RUNNING" : "",
           (flags & 0x10000) ? " MULTICAST" : "");
}

static void show_one(const char *ifname)
{
    char p[128];
    char buf[32];
    printf("%s: ", ifname);
    show_status(ifname);
    printf(" ");
    snprintf(p, sizeof p, "/sys/class/net/%s/address", ifname);
    memset(buf, 0, sizeof buf);
    FILE *f = fopen(p, "r");
    if (f) {
        if (fgets(buf, sizeof buf, f))
            buf[strcspn(buf, "\n")] = 0;
        fclose(f);
    }
    if (buf[0])
        printf("HWaddr %s", buf);
    /* IP 地址(RTM_GETADDR netlink 查询) */
    dump_addr(ifname, if_nametoindex(ifname));
    printf("\n");
}

int main(int argc, char *argv[])
{
    DIR *d = opendir("/sys/class/net");
    if (!d) {
        perror("/sys/class/net");
        return 1;
    }
    struct dirent *e;
    int shown = 0;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, "lo") == 0 || e->d_name[0] == '.')
            continue;
        if (argc > 1 && strcmp(e->d_name, argv[1]) != 0)
            continue;
        show_one(e->d_name);
        shown++;
    }
    closedir(d);
    if (!shown && argc > 1) {
        printf("ifconfig: %s: 不存在\n", argv[1]);
        return 1;
    }
    return 0;
}
