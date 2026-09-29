#!/bin/sh
# pm-site-verify.sh - 断"PM 的默认源就是官方镜像站 www.parlz.com/feed"。
#
# 判据(都取终态, 不看 pm 自己打的字):
#   ① 盘上 /etc/pm/feeds.conf 一行就是 http://www.parlz.com/feed;
#   ② guest 里 PM_FEED 没被开发机地址盖掉(init 的默认值也必须是官网);
#   ③ `pm install pm` 真从官网把包拉下来装上 —— 这条同时验证 DNS 解析、
#      HTTPS + 证书链、http→https 的 301 跳转、以及 .pm 解包安装;
#   ④ 装完 `pm --version` 与官网 feed/Packages 里那一行**版本一致**。
#
# 前置: 宿主能出网(QEMU user NAT 直接转发宿主网络)。
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/pm-site-verify.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
P=/mnt/f/Linux/Parlz
IMG=$P/images
FEED_URL=http://www.parlz.com/feed
W=/home/jgzyes/pm-site-verify
rm -rf "$W"; mkdir -p "$W"
LOG=$W/serial.log
FIFO=$W/in.fifo

for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs"; do
    [ -f "$f" ] || { echo "缺 $f"; exit 1; }
done

echo ">>> [0] 预检: 宿主到官网 feed 通不通(不通就是网络问题, 不是产品问题)"
WANT_VER=$(timeout 20 curl -sL "$FEED_URL/Packages" 2>/dev/null | awk '$1=="pm"{print $2}')
if [ -z "$WANT_VER" ]; then
    echo "  FAIL: 宿主拿不到 $FEED_URL/Packages —— 先确认网络/域名"
    exit 1
fi
echo "    官网 pm 包版本: $WANT_VER"

echo ">>> [1] guest 里核默认源 + 真装一次"
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"
qemu-system-x86_64 -m 1024M -nographic -no-reboot \
    -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
    -append "console=ttyS0,115200 install.skip=1 login.skip=1" \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    <"$FIFO" >"$LOG" 2>&1 &
QP=$!
wait_for() {
    _i=0
    while [ $_i -lt $(( ${2:-60} * 5 )) ]; do
        grep -qa "$1" "$LOG" 2>/dev/null && return 0
        grep -qa "Kernel panic" "$LOG" 2>/dev/null && return 2
        _i=$((_i + 1)); sleep 0.2
    done
    return 1
}
wait_for "type commands directly" 120 || { echo "  没进 shell"; tail -15 "$LOG"; kill $QP; exit 1; }
wait_for "Parlz shell" 60 || echo "  (警告: shell 没起来)"
wait_for "init: network up" 90 || echo "  (警告: 网络没起来, 后面多半要失败)"

# 盘上 feeds.conf 的内容: 直接 cat(parlz-sh 不支持 $(), 别用命令替换)
printf '/bin/busybox cat /etc/pm/feeds.conf\n' >&3
sleep 2
printf 'echo PMFEEDENV=$PM_FEED\n' >&3
sleep 2
printf 'pm install pm\n' >&3
wait_for "pm 安装完成\|已安装" 240 || echo "  (pm install pm 没出完成标记)"
sleep 2
# core.pm 是 17.9 MiB 的大包(默认系统裁掉的命令都在这儿) —— 这条同时验
# 大文件下载与解包; 装完用**被裁的 awk** 真跑一条, 证明"从官网装得回来"。
printf 'pm install core\n' >&3
wait_for "pm 安装完成\|core 安装完成\|已安装" 400 || echo "  (pm install core 没出完成标记)"
sleep 3
# 自研 awk 是子集: 只支持 {print} / {print $N} / /pat/{print}(不支持 BEGIN)。
# 程序必须用**单引号**(parlz-sh 单引号内不展开, 双引号里 $2 会被当变量吃掉)。
printf '%s\n' "/bin/busybox printf 'a 42 b\n' | awk '{print \$2}'" >&3
sleep 3
printf 'pm --version\n' >&3
sleep 4
printf 'echo SITE_DONE\n' >&3
wait_for "SITE_DONE" 60
exec 3>&-
kill $QP 2>/dev/null; wait $QP 2>/dev/null

echo ">>> [2] 判据"
FAIL=""
# 命令回显后面那一行就是文件内容。**认带提示符的那次**回显 —— 早喂进去的
# 输入会被 tty 提前回显一行(还没进 shell), 拿第一处匹配会取到 banner。
FEEDFILE=$(grep -a -A1 'root@parlz.*busybox cat /etc/pm/feeds.conf' "$LOG" \
           | sed -n 2p | tr -d ' \r')
[ "$FEEDFILE" = "$FEED_URL" ] \
    || FAIL="$FAIL /etc/pm/feeds.conf 不是 $FEED_URL(实得 [$FEEDFILE])"
if grep -aq 'PMFEEDENV=.*10\.0\.2\.2' "$LOG"; then
    FAIL="$FAIL PM_FEED 被开发机地址盖住了"
fi
grep -aq "www\.parlz\.com" "$LOG" \
    || FAIL="$FAIL 全程没出现 www.parlz.com(pm 没走官网?)"
GOT_VER=$(grep -a '^pm+' "$LOG" | tail -1 | tr -d ' \r')
# Packages 的版本列不带 "pm+" 前缀(pm+1.1-RC+1 vs 1.1-RC+1), 两种写法都认
case "$GOT_VER" in
    "$WANT_VER"|"pm+$WANT_VER") ;;
    *) FAIL="$FAIL 装完 pm --version=[$GOT_VER] 与官网 [$WANT_VER] 不一致" ;;
esac
# 被裁的 awk 必须真装回来并跑出结果(判据取终态: 输出里有整行 42)
# 串口日志行尾是 \r\n: 先 tr 掉 CR 再整行匹配(注意 grep 的 \r 不是回车转义,
# 写成 '^42\r?$' 会被当成 42r? 而永远配不上 —— 踩过)
tr -d '\r' < "$LOG" | grep -aqx '42' || FAIL="$FAIL pm install core 之后 awk 没跑出 42"
# core 也必须从官网下 —— 但先看**官网那份 feed 是不是旧部署物**: 老 pm.pm 里
# 带过 /etc/pm/feeds.conf(装完把盘上那份盖成开发机地址), 那属于"站点没跟着发布",
# 不是产品缺陷 → 给 WARNING 并跳过这条; 官网更新后这条自动变成硬判据。
LIVE_HAS_CONF=$(mktemp -d)
(cd "$LIVE_HAS_CONF" && timeout 60 curl -sL -o pm.pm "$FEED_URL/pm.pm" 2>/dev/null \
    && cpio -it < pm.pm 2>/dev/null | grep -q 'etc/pm/feeds\.conf' && echo yes > hasconf)
if [ -f "$LIVE_HAS_CONF/hasconf" ]; then
    echo "  ⚠ 官网 feed 是旧部署物(它的 pm.pm 里带着 feeds.conf 会盖掉盘上源) —— 把 output/feed 重新部署到站点后这条会变成硬判据"
else
    CORE_SRC=$(grep -a '从 http[^ ]* 下载 core.pm' "$LOG" | tail -1 | sed 's/.*从 //;s/ 下载.*//')
    [ "$CORE_SRC" = "$FEED_URL" ] \
        || FAIL="$FAIL core 的下载源是 [$CORE_SRC](应为 $FEED_URL)"
fi
rm -rf "$LIVE_HAS_CONF"
# 本地 feed 的正确性(部署源): pm.pm 的**成员表**里不许有 etc/pm/feeds.conf
if [ -f "$P/output/feed/pm.pm" ]; then
    if cpio -it < "$P/output/feed/pm.pm" 2>/dev/null | grep -q 'etc/pm/feeds\.conf'; then
        FAIL="$FAIL output/feed/pm.pm 里带着 etc/pm/feeds.conf(装包会改写源配置; 跑 build-pm-feed.sh)"
    fi
fi

echo "--- 关键行 ---"
grep -a "FEEDFILE=\|PMFEEDENV=\|pm: \|pm+\|SITE_DONE\|安装" "$LOG" | tail -18 | sed 's/^/    /'
if [ -n "$FAIL" ]; then
    echo "pm-site-verify: FAIL ->$FAIL"
    tail -30 "$LOG"
    exit 1
fi
echo "pm-site-verify: PASS(默认源=官网; guest 真从 www.parlz.com/feed 装上了 $GOT_VER)"
