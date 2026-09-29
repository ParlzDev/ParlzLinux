#!/bin/sh
# ifc-verify.sh - 验 ifc 的三条路径 + 配好之后**真的能用**。
#
# 判据设计的原则(踩过: 只看 "OK" 字样会假通过 —— 命令回显本身就含路径):
#   每个用例都断"终态"—— resolv.conf 被 DHCP 改写、默认路由进了 FIB、
#   ping 真收到回包,而不是只 grep 一行 ifc 自己打的 OK。
#
# 用例:
#   A 交付默认路径: cmdline 不带地址 → body 跑 `ifc dhcp`。
#     e1000 + QEMU user NAT(内置 DHCP)应一轮拿到 10.0.2.15/24 gw 10.0.2.2,
#     并把 /etc/resolv.conf 换成 DHCP 给的那份(证明不是烘进 initramfs 的默认)。
#   B 静态旁支: cmdline parlz.ip=/parlz.netmask=/parlz.gw= → 走老静态路径,
#     一轮 link_up → carrier → addr,给"没有 DHCP 的自动化环境"留路。
#   C `-nic none`: 10s 左右放弃, 不能把启动卡满 60s, body 接着打
#     "network config failed"。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/ifc-verify.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
W=/home/jgzyes/ifc-verify
rm -rf "$W"; mkdir -p "$W"

for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs"; do
    [ -f "$f" ] || { echo "缺 $f(先 build-kernel.sh)"; exit 1; }
done

# run_case <名字> <网卡参数> <cmdline 追加> [probe]
#   probe = 起来后往串口喂三条实测命令(resolv.conf / 默认路由 / ping)
run_case() {
    _name=$1; _net=$2; _extra=$3; _probe=${4:-}
    _log=$W/$_name.log
    _fifo=$W/$_name.fifo
    rm -f "$_fifo"; mkfifo "$_fifo"
    echo "--- 用例 $_name ---"
    _t0=$(date +%s)
    # 用 **读写**方式握住写端:纯 3>fifo 会阻塞到出现读者, 而读者(qemu)
    # 要等这一行返回才起 —— 互等死锁(ctrl-c-verify.sh 用的就是 <>)
    exec 3<>"$_fifo"
    # shellcheck disable=SC2086
    qemu-system-x86_64 -m 1024M -nographic -no-reboot \
        -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
        $_net \
        -append "console=ttyS0,115200 install.skip=1 login.skip=1 $_extra" \
        <"$_fifo" >"$_log" 2>&1 &
    _pid=$!
    _i=0
    while [ $_i -lt 450 ]; do
        grep -qa "Parlz boot ready" "$_log" 2>/dev/null && break
        grep -qa "kernel panic" "$_log" 2>/dev/null && break
        _i=$((_i + 1)); sleep 0.2
    done
    _t1=$(date +%s)
    LAST_SECS=$((_t1 - _t0))
    echo "    到 boot ready 用时: ${LAST_SECS}s"
    if [ "$_probe" = probe ]; then
        sleep 2
        printf 'echo MARK-DNS\ncat /etc/resolv.conf\n' >&3
        sleep 2
        printf 'echo MARK-ROUTE\n/bin/busybox grep 00000000 /proc/net/route\n' >&3
        sleep 2
        printf 'echo MARK-PING\n/bin/busybox ping -c 1 -W 2 10.0.2.2\n' >&3
        sleep 6
    fi
    exec 3>&-
    kill $_pid 2>/dev/null || true; wait $_pid 2>/dev/null || true
    grep -a "ifc:\|init: network\|init: configuring\|network config failed" \
        "$_log" | sed 's/^/    /'
}

FAIL=""

# A) 交付默认路径: DHCP
run_case dhcp "-netdev user,id=n0 -device e1000,netdev=n0" "" probe
A_LOG=$W/dhcp.log
A_T=$LAST_SECS
grep -qa "ifc: dhcp: DISCOVER on eth0"      "$A_LOG" || FAIL="$FAIL A:没有发出 DISCOVER"
grep -qa "ifc: dhcp: OFFER 10.0.2.15"       "$A_LOG" || FAIL="$FAIL A:没拿到 OFFER"
grep -qa "ifc: dhcp: eth0 10.0.2.15/24 gw 10.0.2.2" "$A_LOG" \
    || FAIL="$FAIL A:租约没落成地址/网关"
grep -qa "init: network up"                 "$A_LOG" || FAIL="$FAIL A:body 没认成功"
[ "$A_T" -le 30 ] || FAIL="$FAIL A:到 boot ready ${A_T}s(DHCP 拖慢了启动)"
# DHCP 真的重写了 resolv.conf: 烘进 initramfs 的默认里有 8.8.8.8 兜底,
# DHCP 那份只有服务器给的 10.0.2.3 —— 标记之后不该再出现 8.8.8.8
awk '/MARK-DNS/{f=1;next} /MARK-ROUTE/{f=0} f' "$A_LOG" > "$W/dns.txt"
grep -qa "nameserver 10.0.2.3" "$W/dns.txt" \
    || FAIL="$FAIL A:/etc/resolv.conf 没被 DHCP 改写"
if grep -qa "8.8.8.8" "$W/dns.txt"; then
    FAIL="$FAIL A:resolv.conf 还是烘进去的那份(8.8.8.8 兜底仍在)"
fi
# 默认路由进了 FIB
awk '/MARK-ROUTE/{f=1;next} /MARK-PING/{f=0} f' "$A_LOG" > "$W/route.txt"
grep -qa "00000000" "$W/route.txt" \
    || FAIL="$FAIL A:/proc/net/route 里没有默认路由"
# 真的能连通
awk '/MARK-PING/{f=1;next} f' "$A_LOG" > "$W/ping.txt"
grep -qaE "1 packets (transmitted|received)|bytes from" "$W/ping.txt" \
    || FAIL="$FAIL A:ping 10.0.2.2 没收到回包(配好了但连不通)"

# B) 静态旁支: cmdline parlz.ip=
run_case static "-netdev user,id=n0 -device e1000,netdev=n0" \
    "parlz.ip=10.0.2.15 parlz.netmask=255.255.255.0 parlz.gw=10.0.2.2" ""
B_LOG=$W/static.log
grep -qa "init: configuring network via /bin/ifc static" "$B_LOG" \
    || FAIL="$FAIL B:没走静态旁支"
grep -qa "ifc: round 1: carrier(eth0) -> up" "$B_LOG" \
    || FAIL="$FAIL B:没有 carrier up 那行"
grep -qa "ifc: eth0 10.0.2.15/24 gw 10.0.2.2 (round 1)" "$B_LOG" \
    || FAIL="$FAIL B:不是一轮就配好"
grep -qa "init: network up" "$B_LOG" || FAIL="$FAIL B:network up 没打出来"

# C) 没网卡: 不许卡满 60s
run_case no-nic "-nic none" "" ""
C_LOG=$W/no-nic.log
grep -qa "10s 内没有可用的非 lo 接口" "$C_LOG" \
    || FAIL="$FAIL C:没有 10s 放弃那行"
grep -qa "init: network config failed" "$C_LOG" || FAIL="$FAIL C:body 没接着走"
C_T=$(grep -ac . "$C_LOG")
[ "$C_T" -gt 0 ] || FAIL="$FAIL C:日志空"
[ "$LAST_SECS" -le 25 ] || FAIL="$FAIL C:无网卡却等了 ${LAST_SECS}s"

if [ -n "$FAIL" ]; then
    echo "ifc-verify: FAIL ->$FAIL"
    for f in dhcp static no-nic; do
        echo "--- $f 尾 25 行 ---"; tail -25 "$W/$f.log"
    done
    exit 1
fi
echo "ifc-verify: PASS(DHCP 一轮拿到租约并重写 resolv.conf/默认路由/真连通;"
echo "                   静态旁支一轮配好; 无网卡 10s 放弃不卡启动)"
