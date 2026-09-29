#!/bin/sh
# make-release.sh - 出 ParlzOS 的预发布(rc/alpha/beta/release)并把产物收进 output/。
#
# 发布号(单一来源, 三处一致):
#   <时区>+<构建时间>+<构建人>+<阶段>.<大版本>.<小版本>
#   例: CST+0800+20260927-163500+jgzyes@parlz.com+rc.0.1
#   ① 内核 UTS_RELEASE = 7.2.5-parlz-<ID>   → /proc/version 与 uname -r 带原文
#   ② 内核启动横幅第二行 "Parlz release <ID>"
#   ③ rootfs 的 /etc/parlz-release(装到盘上也跟着走)
# 生成物: 仓库根的 .parlz-release(各字段) +
#         linux-7.2.5/include/linux/parlz-release.h(内核侧同一串)
# 之后 build-userland.sh / build-kernel.sh 都读它们, 不用手工传参。
#
# 用法(在 WSL 里跑, 别用 Git Bash 直调 wsl -e sh /mnt/... 会被改路径):
#   wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/make-release.sh"
#   wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/make-release.sh rc 0 1"
# 环境变量:
#   PARLZ_BUILDER  构建人(默认 jgzyes@parlz.com)
#   SKIP_E2E=1     跳过装盘端到端(只出产物 + 版本自检)
#   KEEP_OUTPUT=1  保留 output/ 里上一次发布的文件(默认清空重建)
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

P=/mnt/f/Linux/Parlz
IMG=$P/images
OUT=$P/output
STAGE=${1:-rc}
MAJOR=${2:-0}
MINOR=${3:-1}
BUILDER=${PARLZ_BUILDER:-jgzyes@parlz.com}
# VGA 支持位: 默认**带**(发布号后缀 -F\V)。PARLZ_VGA=0 出无 VGA 版
# (后缀 -UN\V, 且 isolinux.cfg/syslinux.cfg 的 APPEND 里不加 console=tty0)。
# 同一个开关 export 出去给 build-iso.sh / gen-fatboot.sh, 别让号和内容分叉。
export PARLZ_VGA
case "${PARLZ_VGA:-1}" in
    0) VGA_TAG="-UN\V"; PARLZ_VGA=0; VGA_DESC="无 VGA(只串口)" ;;
    *) VGA_TAG="-F\V";  PARLZ_VGA=1; VGA_DESC="VGA(tty0)+串口(ttyS0)" ;;
esac
# PM(包管理器)有**自己**的版本号, 格式: 组件+大.小-阶段+第几版
#   阶段 R=RELEASE  RC=RELEASE CANDIDATE  B=BETA  A=ALPHA
PM_MAJOR=${PM_MAJOR:-1}
PM_MINOR=${PM_MINOR:-1}
PM_TYPE=${PM_TYPE:-RC}
PM_N=${PM_N:-1}
PM_VERSION_ID="pm+$PM_MAJOR.$PM_MINOR-$PM_TYPE+$PM_N"

case "$STAGE" in alpha|beta|rc|release) ;; *) echo "stage 只能是 alpha/beta/rc/release(给了 $STAGE)"; exit 2 ;; esac

TZS=$(date +%Z%z)                      # CST+0800 —— 发布号里的"时区"
BT=$(date +%Y%m%d-%H%M%S)              # 20260927-163500 —— 构建时间(不带空格)
STAMP=$(date)                          # Sun Sep 27 16:35:00 CST 2026 —— 喂 linux_banner
ID="$TZS+$BT+$BUILDER+$STAGE.$MAJOR.$MINOR$VGA_TAG"
REL_FULL="7.2.5-$ID"
[ "${#REL_FULL}" -le 63 ] || { echo "UTS_RELEASE 会超长: $REL_FULL (${#REL_FULL}>63)"; exit 2; }

echo "=== [1/8] 生成发布号 ==="
echo "  ID         : $ID"
echo "  UTS_RELEASE: $REL_FULL (${#REL_FULL}/63)"
cat > "$P/.parlz-release" <<EOF
# 由 scripts/make-release.sh 生成 —— ParlzOS 发布号(单一来源)
PARLZ_VERSION="$MAJOR.$MINOR.0"
PARLZ_MAJOR="$MAJOR"
PARLZ_MINOR="$MINOR"
PARLZ_STAGE="$STAGE"
PARLZ_BUILDER="$BUILDER"
PARLZ_BUILD_TZ="$TZS"
PARLZ_BUILD_TIME="$BT"
PARLZ_VGA="$PARLZ_VGA"
PARLZ_VGA_TAG="$VGA_TAG"
PARLZ_TIMESTAMP="$STAMP"
PARLZ_RELEASE_ID="$ID"
EOF

# PM 自己的版本号(与 ParlzOS 的号独立演进): build-userland 读它编进 /bin/pm,
# build-pm-feed 读它写 Packages 索引的版本列。
cat > "$P/.pm-release" <<EOF
# 由 scripts/make-release.sh 生成 —— PM(Parlz 包管理器)自己的版本号
# 格式: 组件+大.小-阶段+第几版   阶段 R=RELEASE RC=RC B=BETA A=ALPHA
PM_MAJOR="$PM_MAJOR"
PM_MINOR="$PM_MINOR"
PM_TYPE="$PM_TYPE"
PM_N="$PM_N"
PM_VERSION_ID="$PM_VERSION_ID"
EOF
echo "  PM 版本号: $PM_VERSION_ID"

# 落到 C 字符串前把反斜杠双写(-F\V → "-F\\V"), 否则 \V 被当转义序列,
# 启动横幅会打成 "-FV"。
ID_C=$(printf '%s' "$ID" | sed 's/\\/\\\\/g')
cat > "$P/linux-7.2.5/include/linux/parlz-release.h" <<EOF
/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Parlz 发布号 —— 单一来源, 由 scripts/make-release.sh 生成(手工改会被下次发布覆盖)。
 *
 * 发布号格式:  时区 + 构建时间 + 构建人 + 阶段.大版本.小版本[-F\V 或 -UN\V]
 *   PARLZ_RELEASE_ID = "$ID"
 * 内核把同一个串接进 UTS_RELEASE(CONFIG_LOCALVERSION = "-<发布号>", 反斜杠
 * 在 C 字符串里已双写转义), 于是 /proc/version 与 uname -r 里就是它的原文。
 */
#ifndef _LINUX_PARLZ_RELEASE_H
#define _LINUX_PARLZ_RELEASE_H

#define PARLZ_VERSION    "$MAJOR.$MINOR.0"
#define PARLZ_MAJOR      "$MAJOR"
#define PARLZ_MINOR      "$MINOR"
#define PARLZ_STAGE      "$STAGE"                  /* alpha / beta / rc / release */
#define PARLZ_BUILDER    "$BUILDER"
#define PARLZ_BUILD_TZ   "$TZS"                    /* date +%Z%z */
#define PARLZ_BUILD_TIME "$BT"                     /* date +%Y%m%d-%H%M%S */
#define PARLZ_RELEASE_ID "$ID_C"

#endif /* _LINUX_PARLZ_RELEASE_H */
EOF

echo "=== [2/8] 用户空间 + rootfs(含 /etc/parlz-release) ==="
sh "$P/scripts/build-userland.sh" > /home/jgzyes/rel-userland.log 2>&1 \
    || { echo "build-userland 失败:"; tail -20 /home/jgzyes/rel-userland.log; exit 1; }
grep -a "发布号\|userland + initramfs done" /home/jgzyes/rel-userland.log | tail -3
echo "  rootfs 里的 /etc/parlz-release:"
sed 's/^/    /' "/home/jgzyes/parlz-userland/root/etc/parlz-release"

echo "=== [3/8] 内核(发布号进 UTS_RELEASE + banner)+ 引导镜像 ==="
sh "$P/scripts/build-kernel.sh" > /home/jgzyes/rel-kernel.log 2>&1 \
    || { echo "build-kernel 失败:"; tail -25 /home/jgzyes/rel-kernel.log; exit 1; }
grep -a "发布号\|UTS_RELEASE ->\|banner ->\|gen-fatboot: OK" /home/jgzyes/rel-kernel.log | sed 's/^/  /'
# 不起虚拟机也能先确认串真进了内核: linux_banner 在 vmlinux 的 .rodata 里
# (bzImage 是压缩过的, grep 不到)
grep -a "Linux version $REL_FULL ($BUILDER@)" /home/jgzyes/parlz-kernel/vmlinux | head -1 | sed 's/^/  /'
grep -qaF "Linux version $REL_FULL" /home/jgzyes/parlz-kernel/vmlinux \
    && echo "  vmlinux 的 linux_banner 已带发布号  OK" \
    || { echo "  FAIL: vmlinux 里找不到带发布号的 linux_banner"; exit 1; }

echo "=== [4/8] 安装 ISO ==="
sh "$P/scripts/build-iso.sh" > /home/jgzyes/rel-iso.log 2>&1 \
    || { echo "build-iso 失败:"; tail -20 /home/jgzyes/rel-iso.log; exit 1; }
grep -a "build-iso done" /home/jgzyes/rel-iso.log | sed 's/^/  /'

echo "=== [5/8] 版本自检(真启动, 看 /proc/version / uname -r / /etc/parlz-release) ==="
W=/home/jgzyes/relcheck; rm -rf $W; mkdir -p $W
LOG=$W/serial.log; FIFO=$W/in.fifo; mkfifo "$FIFO"
exec 3<>"$FIFO"          # 先占住写端, 否则 qemu 的 <FIFO 与"等日志"互等死锁
qemu-system-x86_64 -m 1024M -nographic -no-reboot -serial mon:stdio \
  -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 install.skip=1 login.skip=1" \
  <"$FIFO" >"$LOG" 2>&1 &
Q=$!
i=0
while [ $i -lt 300 ]; do
    grep -qa "type commands directly" "$LOG" && break
    grep -qa "kernel panic" "$LOG" && break
    i=$((i+1)); sleep 1
done
# 一条一条喂, 每条之间等一下: 一次性灌三行时, shell 在命令执行期间重设
# 终端属性的那一下会把还排在输入队列里的行丢掉(踩过, 见 sh.c 的注释)。
printf 'uname -r\n' >&3;              sleep 8
printf 'cat /proc/version\n' >&3;     sleep 8
printf 'cat /etc/parlz-release\n' >&3; sleep 8
printf 'pm --version\n' >&3;          sleep 8
exec 3>&-; kill $Q 2>/dev/null; wait $Q 2>/dev/null || true
VFAIL=""
grep -a "Parlz 0\.\|Parlz release\|Linux version\|kernel-release\|^pm" "$LOG" | sed 's/^/  /' | head -8
grep -qaF "Parlz release $ID" "$LOG"          || VFAIL="$VFAIL 横幅"
grep -qaF "Linux version $REL_FULL" "$LOG"    || VFAIL="$VFAIL /proc/version"
grep -qa "($BUILDER)" "$LOG"                 || VFAIL="$VFAIL 构建人"
grep -qaF "version: $ID" "$LOG"               || VFAIL="$VFAIL /etc/parlz-release"
grep -qaF "$PM_VERSION_ID" "$LOG"             || VFAIL="$VFAIL pm--version"
[ -z "$VFAIL" ] || { echo "版本自检 FAIL ->$VFAIL, 串口日志尾:"; tail -25 "$LOG"; exit 1; }
echo "  版本自检 PASS: 横幅 / /proc/version / uname -r / /etc/parlz-release / pm --version 同源"

echo "=== [6/8] 端到端装盘验收 ==="
if [ "${SKIP_E2E:-0}" = "1" ]; then
    echo "  SKIP_E2E=1: 跳过 verify-user-install.sh(产物里的盘快照可能不是本轮的)"
else
    sh "$P/scripts/verify-user-install.sh" > /home/jgzyes/rel-e2e.log 2>&1 \
        || { echo "  端到端 FAIL:"; tail -30 /home/jgzyes/rel-e2e.log; exit 1; }
    grep -a "阶段 . PASS\|verify-user-install: PASS" /home/jgzyes/rel-e2e.log | sed 's/^/  /'
fi

echo "=== [7/8] 收产物进 output/ ==="
[ "${KEEP_OUTPUT:-0}" = "1" ] || rm -rf "$OUT"
mkdir -p "$OUT"
PRE="parlz-$STAGE.$MAJOR.$MINOR-$BT"
cp "$IMG/parlz-bzImage"     "$OUT/$PRE-bzImage"
cp "$IMG/parlz-initramfs"   "$OUT/$PRE-initramfs"
cp "$IMG/parlz-bootfat.img" "$OUT/$PRE-bootfat.img"
cp "$IMG/parlz-install.iso" "$OUT/$PRE-install.iso"
[ -f /home/jgzyes/parlz-disk.img ] && cp /home/jgzyes/parlz-disk.img "$OUT/$PRE-installed-disk.img"

# pm 的镜像站根目录: 必须在 output/ 重建**之后**再生成(feed 默认就落在
# output/feed, 早生成会被上面的 rm -rf 连锅端掉)
echo "  --- pm feed(镜像站) ---"
PM_FEED_DIR="$OUT/feed" sh "$P/scripts/build-pm-feed.sh" > /home/jgzyes/rel-feed.log 2>&1 \
    || { echo "  build-pm-feed 失败:"; tail -25 /home/jgzyes/rel-feed.log; exit 1; }
sed 's/^/    /' /home/jgzyes/rel-feed.log | grep -a "pm:\|Packages\|feed\|没有\|!!" | head -14

cat > "$OUT/VERSION.txt" <<EOF
ParlzOS $STAGE 发布
================================
发布号(release id) : $ID
  组成              : 时区($TZS) + 构建时间($BT) + 构建人($BUILDER) + 阶段.大.小($STAGE.$MAJOR.$MINOR)
语义版本(semver)     : $MAJOR.$MINOR.0
PM 版本号            : $PM_VERSION_ID     (guest 里 pm --version / pm version)
镜像站(feed)         : $OUT/feed          (Packages + core.pm + pm.pm + gcc/clang)
内核 release         : $REL_FULL      (/proc/version 与 uname -r 里就是这个)
内核基版             : Linux 7.2.5(就地改造)
构建主机             : $(uname -n) / $(gcc --version | head -1)
构建时间戳           : $STAMP
EOF

cat > "$OUT/RELEASE-NOTES.md" <<EOF
# ParlzOS $STAGE.$MAJOR.$MINOR

发布号: \`$ID\` —— 时区 \`$TZS\` + 构建时间 \`$BT\` + 构建人 \`$BUILDER\` + 阶段 \`$STAGE.$MAJOR.$MINOR\`。

同一个号在三处一致(自检已断言):

| 位置 | 值 |
|---|---|
| \`/proc/version\`、\`uname -r\` | \`$REL_FULL\` |
| 内核启动横幅 | \`Parlz release $ID\` |
| \`/etc/parlz-release\`(装进盘里也跟着走) | \`version: $ID\` |

## 产物

| 文件 | 说明 |
|---|---|
| \`$PRE-bzImage\` | 内核(内嵌本轮 initramfs, 磁盘可自举) |
| \`$PRE-initramfs\` | 用户空间 rootfs(cpio.gz) |
| \`$PRE-bootfat.img\` | 引导分区镜像(64 MiB FAT16: syslinux VBR + vmlinuz + cfg) |
| \`$PRE-install.iso\` | 安装盘(El Torito + isolinux, 可 dd 到 U 盘/刻盘) |
| \`$PRE-installed-disk.img\` | 已装好的整盘(512 MiB), 直接 virtio 挂就能起 |
| \`feed/\` | pm 的镜像站根目录: \`Packages\` + \`core.pm\`(默认系统裁掉的命令) + \`pm.pm\` + \`gcc.pm\`/\`clang.pm\`(有工具链时) |

## 默认系统裁剪与 pm 补装

按 \`userland/pm-trim.list\`, 这批命令**不在**默认系统里(命令名被裁, 但
\`/sbin/busybox\` 本体在, 所以 \`busybox <命令>\` 一直可用):
\`awk、nano、curl/wget、tar、gzip/bzip2/xz 族、sort/head/tail/wc、fdisk/mkfs、\`
\`tree、file、free、dmesg、w3m、pweb、audio、ping、vi\` 等约 260 个命令名。
盘上 \`cat /etc/pm/trimmed-links\` / \`/etc/pm/trimmed-binaries\` 是完整清单。
**默认源就是官方镜像站 \`http://www.parlz.com/feed\`**(盘上 \`/etc/pm/feeds.conf\`
写的就是它), 联网后一条命令补回来:

\`\`\`bash
# guest(默认源, 什么都不用配):
pm available         # 列出官网 feed 里的包
pm install core      # 命令名回来了; pm remove core 干净退回

# 自建/离线镜像时再改源:
#   echo http://<你的服务器>/feed > /etc/pm/feeds.conf
#   # 宿主侧起本地镜像站: PM_REPO=$OUT/feed sh scripts/pm-server.sh
\`\`\`

## 怎么跑

\`\`\`bash
# 从装好的盘启动(必须 virtio: 盘内 syslinux.cfg 写死 root=/dev/vda2)
qemu-system-x86_64 -m 1024M -nographic \\
  -drive file=$PRE-installed-disk.img,if=virtio,format=raw -boot c

# 在系统内装到自己的盘: 装完敲 reboot 直接进装好的系统
PARLZ_FRESH=1 PARLZ_DISK=/path/to/disk.img sh scripts/boot-install.sh   # 然后敲 install
\`\`\`

## 本阶段已知限制

- \`syslinux.cfg\` 不带 \`login.skip\`: 真机首启会要求设置用户名/密码(设计如此)
- UEFI/OVMF 路径缺 \`syslinux.efi\`(宿主没装 SYSLINUX.EFI), Legacy/SeaBIOS 不受影响
- feed(\`pm\`) 的 HTTPS 暂不通(宿主 OpenSSL 静态库待重建), 镜像站先用 \`http://\`
EOF

cd "$OUT"
sha256sum "$PRE-"* feed/core.pm feed/pm.pm feed/Packages > SHA256SUMS.txt 2>/dev/null || sha256sum "$PRE-"* > SHA256SUMS.txt
ls -la "$OUT" | awk 'NR>1 && $5>0 {printf "  %-46s %10d  %s %s\n", $9, $5, $6, $7}'
echo ""
echo "=== 发布完成: $OUT ($STAGE.$MAJOR.$MINOR) ==="
echo "    发布号: $ID"

echo "=== [8/8] 镜像站端到端: guest 从 feed 装回被裁的命令 ==="
PORT=${PM_PORT:-8765}
SRVLOG=/home/jgzyes/rel-pmserver.log
PM_REPO="$OUT/feed" PM_PORT=$PORT nohup sh "$P/scripts/pm-server.sh" > "$SRVLOG" 2>&1 &
SRV=$!
sleep 3
W2=/home/jgzyes/pmcheck; rm -rf $W2; mkdir -p $W2
LOG2=$W2/serial.log; FIFO2=$W2/in.fifo; mkfifo "$FIFO2"
exec 4<>"$FIFO2"
qemu-system-x86_64 -m 1024M -nographic -no-reboot -serial mon:stdio \
  -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 install.skip=1 login.skip=1" \
  <"$FIFO2" >"$LOG2" 2>&1 &
Q2=$!
i=0
while [ $i -lt 300 ]; do
    grep -qa "type commands directly" "$LOG2" && break
    grep -qa "kernel panic" "$LOG2" && break
    i=$((i+1)); sleep 1
done
feed_cmd() { printf '%s\n' "$1" >&4; sleep "$2"; }
feed_cmd "echo http://10.0.2.2:$PORT > /etc/pm/feeds.conf" 3
feed_cmd "pm available" 25
feed_cmd "pm install core" 200
# 断言命令必须是自研 shell 支持的语法: 它**不认 `&&`**(会被当成 test 的操作数),
# 引号内的 `;` 也仍按命令分隔符切(awk 'BEGIN{...; print ...}' 会被劈成两条)。
# 所以每条只做一件事、不带引号内分号。
feed_cmd "nano --version" 8
feed_cmd "file /bin/nano" 8
feed_cmd "wc -c /bin/curl" 8
exec 4>&-
kill $Q2 2>/dev/null; wait $Q2 2>/dev/null
kill $SRV 2>/dev/null; pkill -f "http.server $PORT" 2>/dev/null
grep -a "core.pm\|gcc \|clang \|GNU nano\|ELF\|/bin/curl\|安装\|成员" "$LOG2" | sed 's/^/  /' | head -14
PFAIL=""
grep -qa "core.pm" "$LOG2"         || PFAIL="$PFAIL 索引里看不到 core.pm"
grep -qa "GNU nano" "$LOG2"        || PFAIL="$PFAIL nano 没装回来或跑不起来"
grep -qa "ELF" "$LOG2"             || PFAIL="$PFAIL file 没装回来"
# 要"数字 + 路径"的那行(安装后的 wc 输出), 不能用光 "/bin/curl" —— 回显的
# 命令本身也含这个串, 会假通过。
grep -qaE "[0-9]+ /bin/curl" "$LOG2" || PFAIL="$PFAIL wc 没装回来"
[ -z "$PFAIL" ] || { echo "镜像站端到端 FAIL ->$PFAIL, 串口日志尾:"; tail -25 "$LOG2"; exit 1; }
echo "  镜像站端到端 PASS: guest 从 feed 装回被裁命令并真跑出结果"
