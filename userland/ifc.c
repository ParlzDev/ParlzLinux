/* ifc.c - 配置网络接口:ifc dhcp [ifname|auto] | ifc <ifname|auto> <ip> [netmask] [gateway]
 * 用法: ifc dhcp                      自动选接口并问 DHCP
 *       ifc dhcp eth0                 指定接口走 DHCP
 *       ifc auto 10.0.2.15 255.255.255.0 10.0.2.2   静态(不带 DHCP)
 *       ifc eth0 10.0.2.15 255.255.255.0 10.0.2.2
 *
 * dhcp 模式是**交付介质上的默认路径**:静态地址焊死过一次(10.0.2.15 是 QEMU
 * user-NAT 的地址,在 VMware/真机上照样"配置成功"却谁也连不通)。DHCP 自己
 * 还能带回 DNS,顺手重写 /etc/resolv.conf。
 *
 * ifname 传 'auto' 时:自动找第一个非 lo 接口(轮询 /sys/class/net,
 * 5s 超时)再配置。
 *
 * 底层:RTM_NEWLINK/NEWADDR/NEWROUTE netlink 消息 + DHCP 用 AF_PACKET 裸帧。
 * 接口 UP 之后内核才允许 RTM_NEWADDR;顺序必须是先 link_up 再 addr。
 * 注意:接口在 /sys/class/net 出现不等于已注册进 rtnetlink,
 * 若首次 RTM_NEWADDR 报 ENODEV,重试最多 5 次(间隔 200ms)。
 * 纯 AF_NETLINK 原始套接字,无外部依赖。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <net/if_arp.h>
#ifndef RTA_ALIGNTO
#define RTA_ALIGNTO 4U
#endif
#ifndef RTA_ALIGN
#define RTA_ALIGN(len) ( ((len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1) )
#endif
#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1
#endif

static int nl_fd;
static unsigned int nl_seq;
static const char *nl_last_err;   /* nl_send 失败时的 ACK 错误描述 */

static int nl_open(void)
{
    /* 先不带 NLM_F_ACK,只发不带 ACK 的 RTM_GETLINK 纯查询:
     * 若连查询都收不到 NLMSG_DONE,说明 netlink 通道本身异常 */
    nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nl_fd < 0) {
        perror("socket NETLINK_ROUTE");
        return -1;
    }
    struct sockaddr_nl snl;
    memset(&snl, 0, sizeof snl);
    snl.nl_family = AF_NETLINK;
    /* 必须 connect():它才会给 socket 分配一个专属 portid。
     * 内核把 NLM_F_ACK 的 ACK 投递到"请求方 portid";若 portid 仍是 0
     * (bind() 对 NETLINK_ROUTE 只注册 multicast group,不分配 portid),
     * __netlink_deliver_skb 找不到接收者 -> 回 -EHOSTUNREACH。
     * 所以 NLM_F_ACK(回包)+ connect(分配 portid)二者缺一不可。 */
    if (connect(nl_fd, (struct sockaddr *)&snl, sizeof snl) < 0) {
        perror("connect netlink");
        close(nl_fd);
        return -1;
    }
    nl_seq = 1;
    return 0;
}

static int nl_send(struct nlmsghdr *nh)
{
    if (write(nl_fd, nh, nh->nlmsg_len) < 0) {
        perror("write netlink");
        return -1;
    }
    /* 收 ACK。netlink 回包走内核 socket 内部拷贝,正常毫秒级返回,
     * 2s 超时足够;若超时说明内核没回(请求被丢弃),下轮重发即可 */
    struct timeval tv = { 2, 0 };
    if (setsockopt(nl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
        perror("setsockopt SO_RCVTIMEO");
    char buf[4096];
    struct sockaddr_nl from;
    memset(&from, 0, sizeof from);
    socklen_t fromlen = sizeof from;
    ssize_t n = recvfrom(nl_fd, buf, sizeof buf, 0,
                         (struct sockaddr *)&from, &fromlen);
    if (n < 0) {
        /* 收不到 ACK:可能是 ESTALE(对端退出)或 EAGAIN(超时)。
         * 都重建套接字后返回 -1,主循环下轮重发。 */
        fprintf(stderr, "netlink: 收 ACK 失败 %s,重建套接字\n",
                strerror(errno));
        fflush(stderr);
        close(nl_fd);
        nl_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (nl_fd < 0) {
            perror("socket NETLINK_ROUTE");
            return -1;
        }
        struct sockaddr_nl snl;
        memset(&snl, 0, sizeof snl);
        snl.nl_family = AF_NETLINK;
        if (connect(nl_fd, (struct sockaddr *)&snl, sizeof snl) < 0) {
            perror("connect netlink");
            close(nl_fd);
            return -1;
        }
        nl_last_err = NULL;
        return -1;
    }
    for (struct nlmsghdr *m = (struct nlmsghdr *)buf;
         NLMSG_OK(m, (size_t)n); m = NLMSG_NEXT(m, n)) {
        if (m->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(m);
            if (e->error != 0) {
                /* 内核 ACK:请求被处理,结果错误 */
                nl_last_err = strerror(-e->error);
                fprintf(stderr, "netlink error: %s\n", nl_last_err);
                return -1;
            }
            nl_last_err = NULL;
        }
    }
    nl_last_err = NULL;
    return 0;
}

/* 设接口 UP。
 * 双通道:RTM_NEWLINK(ifi_change=IFF_UP)+ SIOCGIFFLAGS/SIOCSIFFLAGS
 * ioctl 双保险。netlink 设 UP 后 carrier(virtio 特性协商)可能
 * 尚未就绪,网关路由会 ENETDOWN;ioctl 直接翻转 IFF_UP 位更稳。 */
static int link_up(const char *ifname)
{
    int ifidx = if_nametoindex(ifname);
    if (ifidx <= 0) {
        if (ifidx < 0) {
            fprintf(stderr, "ifc: if_nametoindex(%s) 失败,接口未注册\n",
                    ifname);
            fflush(stderr);
        }
        return -1;
    }
    struct {
        struct nlmsghdr nh;
        struct ifinfomsg ifm;
    } msg;
    memset(&msg, 0, sizeof msg);
    msg.nh.nlmsg_len = NLMSG_HDRLEN + sizeof(struct ifinfomsg);
    msg.nh.nlmsg_type = RTM_NEWLINK;
    /* NLM_F_ACK:让内核回 NLMSG_ERROR(成功时 error=0),否则
     * RTM_NEWLINK 这类请求内核不主动回包,recvfrom 会 2s 超时误报失败 */
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    msg.nh.nlmsg_seq = ++nl_seq;
    /* 严格校验模式(rtnetlink strict)下 NEWLINK 头部只允许
     * ifi_family/ifi_index/ifi_change,其余字段必须为 0 */
    msg.ifm.ifi_family = AF_UNSPEC;
    msg.ifm.ifi_index = ifidx;
    msg.ifm.ifi_change = IFF_UP;
    int rc = nl_send(&msg.nh);
    if (rc < 0)
        fprintf(stderr, "ifc: link_up(%s) err=%s\n",
                ifname, nl_last_err ? nl_last_err : "recvfrom");

    /* ioctl 兜底:确认 IFF_UP 确实置上。netlink 成功但 IFF_UP 未生效
     * 时(virtio 特性窗口期),手动翻转;已置上则直接返回。 */
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
        if (ioctl(sfd, SIOCGIFFLAGS, &ifr) == 0) {
            if ((ifr.ifr_flags & IFF_UP) == 0) {
                ifr.ifr_flags |= IFF_UP;
                if (ioctl(sfd, SIOCSIFFLAGS, &ifr) < 0)
                    fprintf(stderr, "ifc: ioctl SIOCSIFFLAGS(%s) 失败: %s\n",
                            ifname, strerror(errno));
                else
                    fprintf(stderr, "ifc: ioctl 强制 UP(%s)\n", ifname);
            }
        } else {
            fprintf(stderr, "ifc: ioctl SIOCGIFFLAGS(%s) 失败: %s\n",
                    ifname, strerror(errno));
        }
        close(sfd);
    }
    return rc;
}

/* 加地址 */
static int addr_add(const char *ifname, const char *ip, int prefix)
{
    int ifidx = if_nametoindex(ifname);
    if (ifidx < 0) {
        perror(ifname);
        return -1;
    }
    struct {
        struct nlmsghdr nh;
        struct ifaddrmsg ifa;
        char attr[64];
    } msg;
    memset(&msg, 0, sizeof msg);
    msg.nh.nlmsg_len = NLMSG_HDRLEN + sizeof(struct ifaddrmsg);
    msg.nh.nlmsg_type = RTM_NEWADDR;
    /* NLM_F_ACK 回包;新内核 strict 模式要求 ifa 头部 ifa_flags=0
     * (flags 经 IFA_FLAGS 属性传递),这里保持 memset 0 */
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE |
                         NLM_F_ACK;
    msg.nh.nlmsg_seq = ++nl_seq;
    msg.ifa.ifa_family = AF_INET;
    msg.ifa.ifa_index = (unsigned int)ifidx;
    msg.ifa.ifa_prefixlen = (unsigned char)prefix;
    /* 属性从 &msg+32 起(ifa 尾部 4B 作对齐 padding,内核跳过),
     * 落在 attr[] 内,不会越界(IFA_LOCAL 8B + IFA_ADDRESS 8B,共 32..48,
     * 在 attr[64] 覆盖范围内)。 */
    unsigned char off = RTA_ALIGN(NLMSG_HDRLEN + sizeof(struct ifaddrmsg));
    struct rtattr *a = (struct rtattr *)((char *)&msg + off);
    a->rta_type = IFA_LOCAL;
    a->rta_len = RTA_LENGTH(sizeof(struct in_addr));
    struct in_addr in;
    if (inet_pton(AF_INET, ip, &in) != 1) {
        fprintf(stderr, "ifc: addr_add: 非法 IP %s\n", ip);
        fflush(stderr);
        return -1;
    }
    memcpy(a + 1, &in, sizeof in);
    off += RTA_ALIGN(a->rta_len);
    a = (struct rtattr *)((char *)&msg + off);
    a->rta_type = IFA_ADDRESS;
    a->rta_len = RTA_LENGTH(sizeof(struct in_addr));
    memcpy(a + 1, &in, sizeof in);
    off += RTA_ALIGN(a->rta_len);
    msg.nh.nlmsg_len = off;
    int rc = nl_send(&msg.nh);
    if (rc < 0 && nl_last_err)
        fprintf(stderr, "ifc: addr_add(%s) err=%s\n", ifname, nl_last_err);
    return rc;
}

/* 加默认路由。
 * 内核 fib_check_nh_v4_gw 对普通网关(非 onlink)做 FIB 查网关路径,
 * 要求网关可达;刚 NEWADDR 完子网路由还没装,查不到 -> EHOSTUNREACH。
 * 标准做法:网关标志加 RTNH_F_ONLINK,跳过网关可达性校验,让内核
 * 直接按链路下一跳处理(QEMU user 网关 10.0.2.2 本就在子网内)。 */
static int route_add(const char *gw, int oifidx)
{
    struct {
        struct nlmsghdr nh;
        struct rtmsg rt;
        char attr[96];
    } msg;
    memset(&msg, 0, sizeof msg);
    msg.nh.nlmsg_len = NLMSG_HDRLEN + sizeof(struct rtmsg);
    msg.nh.nlmsg_type = RTM_NEWROUTE;
    /* NLM_F_ACK:让内核回 NLMSG_ERROR(成功时 error=0),否则
     * RTM_NEWROUTE 这类请求内核不主动回包,recvfrom 会 2s 超时误报失败 */
    msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE |
                         NLM_F_ACK;
    msg.nh.nlmsg_seq = ++nl_seq;
    msg.rt.rtm_family = AF_INET;
    msg.rt.rtm_dst_len = 0;
    msg.rt.rtm_type = RTN_UNICAST;
    msg.rt.rtm_table = RT_TABLE_MAIN;
    /* RTNH_F_ONLINK:强制按链路下一跳,不做网关 FIB 可达性校验。
     * 必须配 RTA_OIF:ONLINK 时内核按 oif 找 dev 设 scope,
     * 没有 oif -> ENODEV("No such device")。 */
    msg.rt.rtm_flags = RTNH_F_ONLINK;
    unsigned char off = RTA_ALIGN(NLMSG_HDRLEN + sizeof(struct rtmsg));
    struct rtattr *a = (struct rtattr *)((char *)&msg + off);
    a->rta_type = RTA_TABLE;
    a->rta_len = RTA_LENGTH(sizeof(int));
    int main_tab = RT_TABLE_MAIN;
    memcpy(a + 1, &main_tab, sizeof main_tab);
    off += RTA_ALIGN(a->rta_len);
    a = (struct rtattr *)((char *)&msg + off);
    a->rta_type = RTA_OIF;
    a->rta_len = RTA_LENGTH(sizeof(int));
    int oif = oifidx;
    memcpy(a + 1, &oif, sizeof oif);
    off += RTA_ALIGN(a->rta_len);
    a = (struct rtattr *)((char *)&msg + off);
    a->rta_type = RTA_GATEWAY;
    a->rta_len = RTA_LENGTH(sizeof(struct in_addr));
    struct in_addr in;
    inet_pton(AF_INET, gw, &in);
    memcpy(a + 1, &in, sizeof in);
    off += RTA_ALIGN(a->rta_len);
    msg.nh.nlmsg_len = off;
    int rc = nl_send(&msg.nh);
    if (rc < 0)
        fprintf(stderr, "ifc: route_add(%s oif=%d) err=%s\n",
                gw, oifidx, nl_last_err ? nl_last_err : "recvfrom");
    return rc;
}

static int prefix_from_mask(const char *mask)
{
    struct in_addr m;
    if (inet_pton(AF_INET, mask, &m) != 1)
        return 24;
    /* s_addr 存的是网络字节序,先转主机序再数前导 1 */
    unsigned int b = ntohl(m.s_addr);
    int p = 0;
    while (b & 0x80000000u) { b <<= 1; p++; }
    return p;
}

/* 等接口出现在 /sys/class/net(超时 5s),返回 1 表示已出现 */
static int wait_if(const char *ifname)
{
    char p[128];
    snprintf(p, sizeof p, "/sys/class/net/%s", ifname);
    for (int i = 0; i < 50; i++) {
        if (access(p, F_OK) == 0)
            return 1;
        usleep(100 * 1000);
    }
    return 0;
}

/* 等 rtnetlink 注册完成:if_nametoindex 成功 + 接口非 DOWN。
 * 驱动 probe 完成前,接口可能还没注册进 rtnetlink。
 * 最多等 10s,每 500ms 重试一次。 */
static int wait_netdev_ready(const char *ifname)
{
    int ready = 0;
    for (int i = 0; i < 20; i++) {
        if (if_nametoindex(ifname) > 0) {
            ready = 1;
            break;
        }
        usleep(500 * 1000);
    }
    return ready;
}

/* 等接口真的 link up(carrier)。
 *
 * 为什么单独等: 内核 e1000/virtio 的 link up 是**异步**的 —— IFF_UP 置上之后
 * 驱动还要协商, 期间 `e1000: eth0 NIC Link is Up` 才出现, 而 ping/网关路由
 * 依赖 IFF_RUNNING(fib_check_nh 会 ENETDOWN)。以前这里既不等、也不出声,
 * 失败就靠主循环盲重试, 表现就是"卡住"。
 *
 * 判据用 /sys/class/net/<if>/carrier(读一个字节)或 ioctl IFF_RUNNING ——
 * **不依赖 netlink 的 link 事件**(那条路要订阅 RTMGRP_LINK, 事件还可能早于
 * 我们订阅而丢掉)。读不到 carrier 文件时退回 ioctl。
 *
 * 返回 1 = 已 up; 0 = 超时(不致命: 地址与子网路由仍然可用, 只是网关可能先失败)。
 * 每秒打印一次进度, 让"等待"看得见。 */
static int wait_running(const char *ifname, int ms)
{
    char p[128];
    snprintf(p, sizeof p, "/sys/class/net/%s/carrier", ifname);
    int waited = 0;
    while (waited <= ms) {
        int up = 0;
        int fd = open(p, O_RDONLY);
        if (fd >= 0) {
            char b = 0;
            if (read(fd, &b, 1) == 1 && b == '1')
                up = 1;
            close(fd);
        } else {
            int sfd = socket(AF_INET, SOCK_DGRAM, 0);
            if (sfd >= 0) {
                struct ifreq ifr;
                memset(&ifr, 0, sizeof ifr);
                snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
                if (ioctl(sfd, SIOCGIFFLAGS, &ifr) == 0 &&
                    (ifr.ifr_flags & IFF_RUNNING))
                    up = 1;
                close(sfd);
            }
        }
        if (up)
            return 1;
        usleep(100 * 1000);
        waited += 100;
        if (waited % 1000 == 0) {
            printf("ifc: 等 %s link up... %d.%ds\n",
                   ifname, waited / 1000, (waited % 1000) / 100);
            fflush(stdout);
        }
    }
    return 0;
}

/* 列出 /sys/class/net 下所有非 lo 接口名(最多 8 个),返回个数。
 * 调用方逐个试 rtnetlink,成功者即当前有效接口。 */
static int list_ifs(char names[8][32])
{
    DIR *d = opendir("/sys/class/net");
    if (!d)
        return 0;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < 8) {
        if (strcmp(e->d_name, "lo") == 0 || e->d_name[0] == '.')
            continue;
        snprintf(names[n], 32, "%s", e->d_name);
        n++;
    }
    closedir(d);
    return n;
}

/* ---------- DHCP:自写客户端(AF_PACKET 裸帧,不依赖内核 IP_PNP) ----------
 *
 * 为什么必须有:交付介质上静态地址焊死过一次 —— 10.0.2.15 / 10.0.2.2 /
 * 10.0.2.3 全是 QEMU user-NAT 的东西,在 VMware/真机上 ifc 照样打
 * "network up",但那块网络谁也连不通,而 pm/curl 的失败点(DNS)离得更远,
 * 极难归因。DHCP 才是"插上就能用"的那条路,而且顺手带回 DNS。
 *
 * 为什么走 AF_PACKET 而不是 UDP 套接字:此刻客户端还没有地址,"发广播收广播"
 * 在 UDP 那层要靠 0.0.0.0/8 路由 + broadcast 路由撑着(行为随内核版本变),
 * 裸帧 + 混杂一条路走到底。代价是自己组 IP/UDP 头、自己算 IP 校验和
 * (UDP 校验和按 RFC 768 在 IPv4 下允许置 0)。
 *
 * 时间上界沿用 link 等待的规矩:有界 + 有判据 + 有声 —— 每轮 DISCOVER 3 s
 * + REQUEST 3 s,共 2 轮,最长约 12 s;选接口那一段最多 10 s。 */

#include <stdint.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC_U     0x63825363u   /* 99.130.83.141,线上按大端读 */

/* BOOTP/DHCP 头(全大端字段,packed 保证布局与 RFC 2131 一致)。
 * options 拉到 332 字节是为了整包凑满 BOOTPAD 最小 576 字节(旧内核/
 * 某些交换机对短 UDP 载荷会丢,填零是合法的,零被当作 pad 跳过)。 */
struct dhcp_bootp {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[332];
} __attribute__((packed));

struct dhcp_iphdr {
    uint8_t  ver_ihl, tos;
    uint16_t total_len, id, frag_off;
    uint8_t  ttl, protocol;
    uint16_t check;
    uint32_t saddr, daddr;
} __attribute__((packed));

struct dhcp_udphdr {
    uint16_t source, dest, len, check;
} __attribute__((packed));

struct dhcp_lease {
    uint32_t ip, mask, gw, server, siaddr;
    uint32_t dns[3];
    int      ndns;
    char     domain[64];
};

static uint16_t ip_cksum(const void *data, int len)
{
    const unsigned short *p = data;
    unsigned long sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len)
        sum += (unsigned long)*(const unsigned char *)p;
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint32_t next_xid(void)
{
    static uint32_t x;
    if (!x) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        x = (uint32_t)(tv.tv_sec ^ tv.tv_usec) ^ ((uint32_t)getpid() << 11);
        if (!x)
            x = 0x12345678u;
    }
    return ++x;
}

static int iface_mac(const char *ifname, unsigned char mac[6])
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    int rc = -1;
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
        memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
        rc = 0;
    } else {
        fprintf(stderr, "ifc: dhcp: 取不到 %s 的 MAC: %s\n",
                ifname, strerror(errno));
    }
    close(fd);
    return rc;
}

/* AF_PACKET 数据报套接字:发送时内核按 sll_addr 替我们补以太头,
 * 接收时以太头已剥掉(载荷从 IP 头开始)。混杂模式保证"还没配地址"
 * 时也收得到发往 ff:ff:ff:ff:ff:ff 的应答。 */
static int dhcp_open_socket(int ifindex)
{
    int fd = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
    if (fd < 0) {
        perror("ifc: dhcp: socket(AF_PACKET)");
        return -1;
    }
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof sll);
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = ifindex;
    sll.sll_protocol = htons(ETH_P_IP);
    if (bind(fd, (struct sockaddr *)&sll, sizeof sll) < 0) {
        perror("ifc: dhcp: bind(ifindex)");
        close(fd);
        return -1;
    }
    struct packet_mreq mr;
    memset(&mr, 0, sizeof mr);
    mr.mr_ifindex = ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
                   &mr, sizeof mr) < 0)
        fprintf(stderr, "ifc: dhcp: 混杂模式打开失败(%s),仍继续\n",
                strerror(errno));
    /* 收包按 300ms 一片,便于自己用时钟计超时并每秒出声 */
    struct timeval tv = { 0, 300000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static long elapsed_ms(const struct timeval *t0)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (long)(now.tv_sec - t0->tv_sec) * 1000 +
           (long)(now.tv_usec - t0->tv_usec) / 1000;
}

/* type:1=DISCOVER 3=REQUEST。requested/server_id 仅 REQUEST 用。 */
static int dhcp_send(int fd, int ifindex, const unsigned char *mac,
                     uint32_t xid, uint8_t type, uint32_t requested,
                     uint32_t server_id)
{
    struct {
        struct dhcp_iphdr  ip;
        struct dhcp_udphdr udp;
        struct dhcp_bootp  b;
    } pkt;
    memset(&pkt, 0, sizeof pkt);

    uint8_t *o = pkt.b.options;
    int ol = 0;
    o[ol++] = 53; o[ol++] = 1; o[ol++] = type;           /* DHCP Message Type */
    o[ol++] = 61; o[ol++] = 7; o[ol++] = 1;              /* Client Identifier */
    memcpy(o + ol, mac, 6); ol += 6;
    o[ol++] = 55; o[ol++] = 5;                           /* Parameter Request */
    o[ol++] = 1; o[ol++] = 3; o[ol++] = 6; o[ol++] = 15; o[ol++] = 28;
    if (requested) {
        o[ol++] = 50; o[ol++] = 4;                       /* Requested IP */
        memcpy(o + ol, &requested, 4); ol += 4;
    }
    if (server_id) {
        o[ol++] = 54; o[ol++] = 4;                       /* Server Identifier */
        memcpy(o + ol, &server_id, 4); ol += 4;
    }
    o[ol++] = 255;

    pkt.b.op = 1;                    /* BOOTREQUEST */
    pkt.b.htype = 1;                 /* Ethernet */
    pkt.b.hlen = 6;
    pkt.b.xid = htonl(xid);
    pkt.b.flags = htons(0x8000);     /* 要求服务器广播回包(客户端还没地址) */
    memcpy(pkt.b.chaddr, mac, 6);
    pkt.b.magic = htonl(DHCP_MAGIC_U);

    pkt.ip.ver_ihl = 0x45;
    pkt.ip.total_len = htons((uint16_t)sizeof pkt);
    pkt.ip.id = htons((uint16_t)xid);
    pkt.ip.frag_off = htons(0x4000); /* DF */
    pkt.ip.ttl = 64;
    pkt.ip.protocol = 17;            /* UDP */
    pkt.ip.daddr = 0xffffffffu;      /* 255.255.255.255 */
    pkt.ip.check = ip_cksum(&pkt.ip, 20);
    pkt.udp.source = htons(DHCP_CLIENT_PORT);
    pkt.udp.dest = htons(DHCP_SERVER_PORT);
    pkt.udp.len = htons((uint16_t)(8 + sizeof pkt.b));
    pkt.udp.check = 0;               /* IPv4 下允许(RFC 768) */

    struct sockaddr_ll dst;
    memset(&dst, 0, sizeof dst);
    dst.sll_family = AF_PACKET;
    dst.sll_protocol = htons(ETH_P_IP);
    dst.sll_ifindex = ifindex;
    dst.sll_halen = 6;
    memset(dst.sll_addr, 0xff, 6);
    dst.sll_pkttype = 3;             /* PACKET_BROADCAST */
    if (sendto(fd, &pkt, sizeof pkt, 0,
               (struct sockaddr *)&dst, sizeof dst) < 0) {
        fprintf(stderr, "ifc: dhcp: 发送失败: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* 收一条属于本次交换的 DHCP 应答;out_type 回填 53 号选项。 */
static int dhcp_recv(int fd, uint32_t xid, const unsigned char *mac, int ms,
                     uint8_t *out_type, struct dhcp_lease *ls)
{
    unsigned char buf[1518];
    struct timeval t0;
    gettimeofday(&t0, NULL);
    int next_report = 1000;
    for (;;) {
        long spent = elapsed_ms(&t0);
        if (spent >= ms)
            return -1;
        if (spent >= next_report) {
            printf("ifc: dhcp: 等应答… %ld.%lds\n",
                   spent / 1000, (spent % 1000) / 100);
            fflush(stdout);
            next_report += 1000;
        }
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR || errno == ETIMEDOUT)
                continue;
            perror("ifc: dhcp: recv");
            return -1;
        }
        if (n < 28)
            continue;
        const struct dhcp_iphdr *ip = (const struct dhcp_iphdr *)buf;
        if ((ip->ver_ihl >> 4) != 4)
            continue;
        int ihl = (ip->ver_ihl & 0x0f) * 4;
        if (ihl < 20 || n < ihl + 8 || ip->protocol != 17)
            continue;
        const struct dhcp_udphdr *up = (const struct dhcp_udphdr *)(buf + ihl);
        if (ntohs(up->dest) != DHCP_CLIENT_PORT)
            continue;
        int pay = (int)n - ihl - 8;
        if (pay < 244)
            continue;
        const struct dhcp_bootp *b = (const struct dhcp_bootp *)(buf + ihl + 8);
        if (b->op != 2 || b->hlen != 6 ||
            memcmp(b->chaddr, mac, 6) != 0 ||
            ntohl(b->xid) != xid || ntohl(b->magic) != DHCP_MAGIC_U)
            continue;

        uint8_t mtype = 0;
        ls->ip = b->yiaddr;
        ls->siaddr = b->siaddr;
        const uint8_t *p = b->options;
        int left = pay - 244;
        while (left > 0 && *p != 255) {
            if (*p == 0) { p++; left--; continue; }   /* pad */
            if (left < 2)
                break;
            int t = p[0], l = p[1];
            p += 2; left -= 2;
            if (l > left)
                break;
            switch (t) {
            case 53: if (l >= 1) mtype = p[0]; break;
            case 1:  if (l >= 4) memcpy(&ls->mask, p, 4); break;
            case 3:  if (l >= 4) memcpy(&ls->gw, p, 4); break;
            case 54: if (l >= 4) memcpy(&ls->server, p, 4); break;
            case 6:
                for (int i = 0; i + 4 <= l && ls->ndns < 3; i += 4) {
                    memcpy(&ls->dns[ls->ndns], p + i, 4);
                    ls->ndns++;
                }
                break;
            case 15:
                if (l > 0 && l < (int)sizeof ls->domain) {
                    memcpy(ls->domain, p, (size_t)l);
                    ls->domain[l] = 0;
                }
                break;
            default: break;
            }
            p += l; left -= l;
        }
        *out_type = mtype;
        return 0;
    }
}

static int mask_prefix_be(uint32_t mask_be)
{
    unsigned int b = ntohl(mask_be);
    int p = 0;
    while (b & 0x80000000u) { b <<= 1; p++; }
    return (p >= 8 && p <= 32) ? p : 24;
}

/* DHCP 给的 DNS 覆盖烘进 initramfs 的那份 QEMU 默认值。
 * options timeout/attempts 是给 glibc 解析器的:默认 5s×2 次才换下一个
 * nameserver,前面挂一个死的就会把 curl 拖成"看着像卡死"。 */
static void resolv_write(const struct dhcp_lease *ls)
{
    FILE *f = fopen("/etc/resolv.conf", "w");
    if (!f) {
        fprintf(stderr, "ifc: dhcp: 写 /etc/resolv.conf 失败: %s\n",
                strerror(errno));
        return;
    }
    for (int i = 0; i < ls->ndns; i++) {
        char s[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ls->dns[i], s, sizeof s);
        fprintf(f, "nameserver %s\n", s);
    }
    if (ls->ndns == 0) {
        fprintf(f, "nameserver 8.8.8.8\nnameserver 1.1.1.1\n");
    }
    if (ls->domain[0])
        fprintf(f, "search %s\n", ls->domain);
    fprintf(f, "options timeout:1 attempts:1\n");
    fclose(f);
}

static int dhcp_apply(const char *ifname, const struct dhcp_lease *ls)
{
    char ips[INET_ADDRSTRLEN], gps[INET_ADDRSTRLEN];
    if (ls->ip == 0) {
        fprintf(stderr, "ifc: dhcp: 应答里没有 yiaddr\n");
        return -1;
    }
    inet_ntop(AF_INET, &ls->ip, ips, sizeof ips);
    uint32_t mask = ls->mask ? ls->mask : htonl(0xffffff00u);
    int prefix = mask_prefix_be(mask);
    if (addr_add(ifname, ips, prefix) < 0) {
        fprintf(stderr, "ifc: dhcp: 配置 %s/%d 失败\n", ips, prefix);
        return -1;
    }
    int gw_fail = 0;
    if (ls->gw) {
        inet_ntop(AF_INET, &ls->gw, gps, sizeof gps);
        wait_running(ifname, 3000);
        for (int w = 0; w < 4; w++) {
            if (route_add(gps, if_nametoindex(ifname)) == 0)
                break;
            gw_fail = 1;
            usleep(700 * 1000);
        }
    } else {
        gps[0] = 0;
    }
    resolv_write(ls);
    char d0[INET_ADDRSTRLEN] = "";
    if (ls->ndns > 0)
        inet_ntop(AF_INET, &ls->dns[0], d0, sizeof d0);
    printf("ifc: dhcp: %s %s/%d%s%s%s dns %s\n", ifname, ips, prefix,
           gps[0] ? " gw " : "", gps, gw_fail ? "(网关未加)" : "",
           d0[0] ? d0 : "无(用 8.8.8.8)");
    fflush(stdout);
    return 0;
}

/* 单接口上的完整交换:DISCOVER → OFFER → REQUEST → ACK */
static int dhcp_try(const char *ifname, int round)
{
    link_up(ifname);
    if (!wait_running(ifname, round == 0 ? 5000 : 1500)) {
        printf("ifc: dhcp: %s 没有 carrier,仍试一次\n", ifname);
        fflush(stdout);
    }
    unsigned char mac[6];
    if (iface_mac(ifname, mac) < 0)
        return -1;
    int idx = if_nametoindex(ifname);
    if (idx <= 0)
        return -1;
    int fd = dhcp_open_socket(idx);
    if (fd < 0)
        return -1;

    int rc = -1;
    for (int attempt = 0; attempt < 2 && rc < 0; attempt++) {
        uint32_t xid = next_xid();
        printf("ifc: dhcp: DISCOVER on %s(第 %d 次, xid %08x)\n",
               ifname, attempt + 1, xid);
        fflush(stdout);
        if (dhcp_send(fd, idx, mac, xid, 1, 0, 0) < 0)
            break;
        struct dhcp_lease offer;
        memset(&offer, 0, sizeof offer);
        uint8_t type = 0;
        if (dhcp_recv(fd, xid, mac, 3000, &type, &offer) < 0) {
            printf("ifc: dhcp: 3s 内没有 OFFER(%s)\n", ifname);
            fflush(stdout);
            continue;
        }
        char oip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &offer.ip, oip, sizeof oip);
        printf("ifc: dhcp: OFFER %s -> REQUEST\n", oip);
        fflush(stdout);
        uint32_t server = offer.server ? offer.server : offer.siaddr;
        if (dhcp_send(fd, idx, mac, xid, 3, offer.ip, server) < 0)
            break;
        struct dhcp_lease ack;
        memset(&ack, 0, sizeof ack);
        type = 0;
        if (offer.mask && offer.gw)       /* 有些服务器 ACK 里不再重复选项 */
            ack = offer;
        if (dhcp_recv(fd, xid, mac, 3000, &type, &ack) < 0) {
            printf("ifc: dhcp: 3s 内没有 ACK(%s)\n", ifname);
            fflush(stdout);
            continue;
        }
        if (type == 6) {
            printf("ifc: dhcp: 服务器回了 NAK,重来\n");
            fflush(stdout);
            continue;
        }
        if (!ack.ip)
            ack.ip = offer.ip;
        rc = dhcp_apply(ifname, &ack);
        break;
    }
    close(fd);
    return rc;
}

/* dhcp 入口:auto 时轮询选接口(最多 10s,与静态路径同规矩),逐个接口试。 */
static int dhcp_main(int auto_mode, const char *want_if)
{
    if (nl_open() < 0)
        return 1;
    int ok = 0;
    int saw_if = 0;
    char last[32] = "";
    for (int round = 0; round < 10 && !ok; round++) {
        char names[8][32];
        int nn = list_ifs(names);
        if (nn == 0) {
            printf("ifc: dhcp: round %d: /sys/class/net 无接口,等...\n",
                   round + 1);
            fflush(stdout);
            usleep(1000 * 1000);
            continue;
        }
        saw_if = 1;
        for (int i = 0; i < nn && !ok; i++) {
            const char *n1 = names[i];
            /* 指名接口时只试它(名字还没出现在 sysfs 就下一轮再看) */
            if (!auto_mode && strcmp(n1, want_if) != 0)
                continue;
            if (if_nametoindex(n1) == 0) {
                if (round % 5 == 0) {
                    printf("ifc: %s 未注册进 rtnetlink,等待...\n", n1);
                    fflush(stdout);
                }
                continue;
            }
            snprintf(last, sizeof last, "%s", n1);
            if (dhcp_try(n1, round) == 0)
                ok = 1;
        }
        if (!ok && round < 9)
            usleep(700 * 1000);
    }
    if (!ok) {
        if (!saw_if)
            printf("ifc: dhcp: 10s 内没有可用的非 lo 接口, 放弃"
                   "(可手动 ifc eth0 <ip> <mask> [gw])\n");
        else
            printf("ifc: dhcp: 10s 内没拿到租约(最近: %s)—— 可在 shell 里"
                   "手动 ifc eth0 <ip> <mask> [gw]\n",
                   last[0] ? last : "接口未注册进 rtnetlink");
        fflush(stdout);
    }
    close(nl_fd);
    return ok ? 0 : 1;
}

int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "dhcp") == 0) {
        const char *want = (argc > 2) ? argv[2] : "";
        int auto_mode = (argc <= 2 || strcmp(want, "auto") == 0);
        return dhcp_main(auto_mode, want);
    }
    if (argc < 3) {
        fprintf(stderr,
            "usage: ifc dhcp [ifname|auto]\n"
            "       ifc <ifname|auto> <ip> [netmask] [gateway]\n"
            "  dhcp 向链路里的服务器要地址(顺带重写 /etc/resolv.conf)\n"
            "  ifname 用 'auto' 时自动选第一个非 lo 接口\n"
            "  例: ifc eth0 10.0.2.15 255.255.255.0 10.0.2.2\n");
        return 1;
    }
    char ifname[32] = "";
    const char *ip = argv[2];
    int prefix = (argc > 3) ? prefix_from_mask(argv[3]) : 24;
    const char *gw = (argc > 4) ? argv[4] : NULL;
    int auto_mode = (strcmp(argv[1], "auto") == 0);

    if (!auto_mode) {
        snprintf(ifname, sizeof ifname, "%s", argv[1]);
        if (!wait_if(ifname)) {
            fprintf(stderr, "ifc: %s 未出现(5s 超时)\n", ifname);
            return 1;
        }
    }

    if (nl_open() < 0)
        return 1;

    /* 诊断:对 lo 发 RTM_NEWLINK(UP),验证 netlink 通道本身。
     * lo 的 ifindex 恒为 1,任何正常内核都应成功 ACK;
     * 若这一步就失败,说明 netlink 套接字/内核通道异常,
     * 不是 virtio 的问题。 */
    {
        struct {
            struct nlmsghdr nh;
            struct ifinfomsg ifm;
        } lo_msg;
        memset(&lo_msg, 0, sizeof lo_msg);
        lo_msg.nh.nlmsg_len = NLMSG_HDRLEN + sizeof(struct ifinfomsg);
        lo_msg.nh.nlmsg_type = RTM_NEWLINK;
        lo_msg.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        lo_msg.nh.nlmsg_seq = ++nl_seq;
        lo_msg.ifm.ifi_family = AF_UNSPEC;
        lo_msg.ifm.ifi_index = 1;
        lo_msg.ifm.ifi_change = IFF_UP;
        int lorc = nl_send(&lo_msg.nh);
        printf("ifc: 诊断 RTM_NEWLINK(lo): %s\n",
               lorc == 0 ? "OK" : nl_last_err ? nl_last_err : "未知");
        fflush(stdout);
    }

    int ok = 0;
    const char *bail = NULL;   /* 提前放弃的原因(有的话) */
    /* 总重试 60s:接口注册/重命名/驱动 probe 都可能晚。
     * 每轮列出 sysfs 全部非 lo 接口,逐个试 link_up + addr_add,
     * 成功即用该接口。 */
    char last_if[32] = "";
    int announced_fail = 0;
    for (int round = 0; round < 60 && !ok; round++) {
        char names[8][32];
        int nn = list_ifs(names);
        if (nn == 0) {
            if (round % 10 == 0)
                printf("ifc: round %d: /sys/class/net 无接口,等...\n", round + 1);
            fflush(stdout);
            /* auto 模式没有接口就是没有(例如 QEMU -nic none): 别把启动卡满 60s,
             * 给 10s 就放弃, 让 shell 早点出来 */
            if (auto_mode && round >= 9) {
                bail = "10s 内没有可用的非 lo 接口";
                printf("ifc: auto: 10s 内没有可用的非 lo 接口, 放弃"
                       "(可手动 ifc eth0 <ip> <mask> [gw])\n");
                fflush(stdout);
                break;
            }
            usleep(1000 * 1000);
            continue;
        }
        if (round == 0) {
            printf("ifc: round 1: 接口 ");
            for (int i = 0; i < nn; i++)
                printf("%s%s", names[i], i + 1 < nn ? " " : "");
            printf("\n");
            fflush(stdout);
        }
        /* 非 auto 且指定名仍有效时优先用它 */
        int start = 0;
        if (!auto_mode && if_nametoindex(ifname) > 0) {
            for (int i = 0; i < nn; i++)
                if (!strcmp(names[i], ifname)) { start = i; break; }
        }
        for (int i = 0; i < nn; i++) {
            int idx = (start + i) % nn;
            char *n1 = names[idx];
            if (if_nametoindex(n1) == 0) {
                /* eth0 在 sysfs 里但尚未注册进 rtnetlink
                 * (rename 窗口期),每 15 轮报一次 */
                if (round % 15 == 0) {
                    printf("ifc: %s 未注册进 rtnetlink(rename 窗口?),等待...\n",
                           n1);
                    fflush(stdout);
                }
                continue;
            }
            if (last_if[0] && strcmp(n1, last_if) != 0) {
                printf("ifc: trying %s\n", n1);
                fflush(stdout);
            }
            snprintf(last_if, sizeof last_if, "%s", n1);
            int lup = link_up(n1);
            if (round == 0)
                printf("ifc: round 1: link_up(%s) -> %s\n", n1,
                       lup >= 0 ? "OK" :
                       nl_last_err ? nl_last_err : "recvfrom");
            fflush(stdout);
            int aok = 0;
            if (lup >= 0) {
                /* IFF_UP 置上 ≠ link up: 等 carrier(首轮给足 5s, 之后 1.5s),
                 * 期间每秒报一次进度, 不再"静默卡住" */
                int run = wait_running(n1, round == 0 ? 5000 : 1500);
                if (round == 0) {
                    printf("ifc: round 1: carrier(%s) -> %s\n", n1,
                           run ? "up" : "超时(继续配 IP, 网关稍后重试)");
                    fflush(stdout);
                }
                for (int try = 0; try < 5; try++) {
                    if (addr_add(n1, ip, prefix) == 0) {
                        aok = 1;
                        break;
                    }
                    if (round == 0)
                        printf("ifc: round 1: addr_add(%s) err=%s\n",
                               n1, nl_last_err ? nl_last_err : "?");
                    if (nl_last_err &&
                        (round + 1) % 10 == 0 && announced_fail < 4) {
                        /* 每 10s 报一次 addr 内核错误,避免刷屏 */
                        printf("ifc: %s addr_add err=%s (round %d)\n",
                               n1, nl_last_err, round + 1);
                        fflush(stdout);
                        announced_fail++;
                    }
                    usleep(300 * 1000);
                }
            }
            if (round == 0)
                printf("ifc: round 1: addr -> %s\n",
                       aok ? "OK" : "FAIL");
            if (aok) {
                /* 接口已配好 IP。网关路由重试:内核要求网关可达,
                 * 刚 NEWADDR 完子网 FIB 条目延迟安装,等 3s 再试,
                 * 3 次间隔 2s;失败不阻塞(接口有 IP,子网内直连可用)。 */
                int gw_fail = 0;
                if (gw) {
                    /* 网关路由要求接口 RUNNING, 且刚 NEWADDR 完子网 FIB 条目
                     * 要一会儿才装好 —— 按 carrier 判据等, 不再盲等 3s */
                    wait_running(n1, 3000);
                    for (int w = 0; w < 4; w++) {
                        if (route_add(gw, if_nametoindex(n1)) == 0)
                            break;
                        gw_fail = 1;
                        usleep(700 * 1000);
                    }
                }
                ok = 1;
                printf("ifc: %s %s/%d%s%s%s (round %d)\n",
                       n1, ip, prefix, gw ? " gw " : "", gw ? gw : "",
                       gw && gw_fail ? "(网关未加,可在 shell 重试)" : "",
                       round + 1);
                fflush(stdout);
                break;
            } else if (round % 30 == 0 && announced_fail < 2 && lup < 0) {
                /* link_up 失败每 30s 报一次 */
                printf("ifc: %s link_up 失败 err=%s (round %d)\n",
                       n1, nl_last_err ? nl_last_err : "?", round + 1);
                fflush(stdout);
                announced_fail++;
            }
        }
        if (!ok) {
            /* 每 5 轮报一次"还在试 + 原因": 60 轮里至少 12 行,
             * 屏幕上不会再出现"round 1 之后一片安静"的假死样子 */
            if ((round + 1) % 5 == 0) {
                printf("ifc: 还在试(%d/60, 最近: %s)\n", round + 1,
                       nl_last_err ? nl_last_err : "link/地址未就绪");
                fflush(stdout);
            }
            usleep(700 * 1000);
        }
    }
    if (!ok) {
        if (bail)
            printf("ifc: 放弃 —— %s\n", bail);
        else
            printf("ifc: 60s 内配置失败(试到: %s)\n",
                   last_if[0] ? last_if : "无接口");
        fprintf(stderr, "ifc: 配置 %s 失败(%s)\n", ifname,
                bail ? bail : "驱动未就绪/名字变化");
        close(nl_fd);
        return 1;
    }
    close(nl_fd);
    return 0;
}
