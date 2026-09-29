#!/bin/sh
# check-installed-root.sh - 宿主侧复核"装到磁盘上的那个根"是否真的完整可用。
#
# 只看串口日志会被骗: pivot_root 成功不代表换过去的是个能跑的系统
# —— 曾经装出来的分区 2 里只有 cdrom 和 lost+found, 日志里照样
# "pivot_root OK", 然后立刻 "(shell exited)"。这里把盘的分区 2 直接
# 挂回来, 用宿主工具核对命令/软链/权限。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh \
#           /mnt/f/Linux/Parlz/scripts/check-installed-root.sh <盘镜像>
#   (默认 /home/jgzyes/parlz-disk.img, 即 boot-install.sh 的目标盘)
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

D=${1:-/home/jgzyes/parlz-disk.img}
M=/mnt/parlz-check
[ -f "$D" ] || { echo "FAIL: 没有盘镜像 $D"; exit 1; }

LOOP=$(losetup -Prf --show "$D") || { echo "FAIL: losetup 失败"; exit 1; }
# loop 设备名以数字结尾, 分区节点带 p 分隔符(/dev/loop0p2, 不是 /dev/loop02)
ROOTDEV="${LOOP}p2"
umount "$M" 2>/dev/null
mkdir -p "$M"

FAIL=""
trap 'umount "$M" 2>/dev/null; losetup -d "$LOOP" 2>/dev/null' EXIT

echo "=== 目标盘 $D -> $LOOP (分区 2 = $ROOTDEV) ==="
echo "--- 分区表 ---"
fdisk -l "$D" 2>/dev/null | sed -n '/Disklabel type/,$p' | head -8

echo "--- e2fsck(装好的根) ---"
E2=$(e2fsck -fn "$ROOTDEV" 2>&1)
echo "$E2" | tail -4
# 只留真报警的行: 版本行 / "Pass N: Checking ..." / 末尾 "组数/块数" 小结
# 都不算问题(注意别拿 "group|inode" 这种词去 grep, 会正好匹到 Pass 标题行)。
PROB=$(echo "$E2" | grep -vE "^e2fsck |^Pass [0-9]|files .*blocks|Group count|^$")
# **计数类**告警放行: "Free inodes/blocks count wrong (…, counted=…) + Fix? no"
# 是"被 kill 的 guest 没来得及把超级块计数写回"的必然结果 —— 阶段 2 是直接
# kill QEMU(等价拔电源), 页缓存/内存里的计数器就停在那儿; e2fsck -p 一条就修好,
# 不丢数据。其余任何输出(位图差异、链接数错、目录项错、i_size 不对…)仍然算问题,
# 别把这条放行扩成"计数以外也放过"。
PROB=$(echo "$PROB" | grep -vE "^(Free inodes count wrong|Free blocks count wrong) \(")
PROB=$(echo "$PROB" | grep -vE "^Fix\? no$")
[ -n "$PROB" ] && { echo "e2fsck 告警:"; echo "$PROB" | head -8; FAIL="$FAIL e2fsck"; }

echo "--- 挂载复核 ---"
mount -o ro "$ROOTDEV" "$M" || { echo "FAIL: 分区 2 挂不上"; exit 1; }

N=$(find "$M" -mindepth 1 | wc -l)
echo "条目数: $N"
[ "$N" -gt 120 ] || FAIL="$FAIL 条目太少($N)"

# 默认系统该留下的东西。注意: awk/nano/curl/tar 等已按 pm-trim.list 裁掉,
# 改由 `pm install core` 提供 —— 它们**不能**当必需项检查。
for p in bin/parlz-sh bin/sh bin/bash bin/install bin/cpfs bin/login \
         bin/mount bin/umount bin/ifc bin/cat bin/ls sbin/busybox; do
    [ -x "$M/$p" ] || { echo "  FAIL: $p 不存在或不可执行"; FAIL="$FAIL 缺$p"; }
done
for p in etc/passwd etc/inittab etc/group etc/parlz-release; do
    [ -f "$M/$p" ] || { echo "  FAIL: 缺文件 $p"; FAIL="$FAIL 缺$p"; }
done
# 凭证文件: 没经过首启的盘上可以没有(那时候还没有账户), 但**只要在**就必须是
# $6$ 散列 —— 明文那版是老格式, 不该再出现在盘上(它是公开下载物)。
if [ -f "$M/etc/parlz-auth" ]; then
    grep -qaE '^[^#][^:]*:\$6\$' "$M/etc/parlz-auth" \
        || { echo "  FAIL: /etc/parlz-auth 里没有 \$6\$ 账户行"; FAIL="$FAIL 凭证明文"; }
else
    echo "  (注: 盘上还没有 /etc/parlz-auth —— 没跑过首启的盘属正常)"
fi
# 裁剪清单要跟着盘走: 装了系统的机器上能查到"哪些命令得 pm 装"
[ -f "$M/etc/pm/trimmed-links" ] \
    || { echo "  FAIL: 缺 /etc/pm/trimmed-links(裁剪清单没进盘)"; FAIL="$FAIL 缺裁剪清单"; }
for g in nano awk curl tar; do
    if [ -e "$M/bin/$g" ]; then
        echo "  FAIL: /bin/$g 应该已裁掉(由 pm 提供)"; FAIL="$FAIL 没裁$g"
    fi
done
# 顶层该有的目录(不含 /lib: 工具链包是可选产物, 缺了不影响系统可用)
for d in dev proc sys tmp mnt root var usr bin sbin etc boot; do
    [ -d "$M/$d" ] || { echo "  FAIL: 缺目录 /$d"; FAIL="$FAIL 缺/$d"; }
done

L=$(readlink "$M/bin/busybox" 2>/dev/null)
[ "$L" = "../sbin/busybox" ] || { echo "  FAIL: /bin/busybox 软链不对(->$L)"; FAIL="$FAIL busybox链"; }
[ -s "$M/bin/busybox" ] || { echo "  FAIL: 经 /bin/busybox 读不到内容"; FAIL="$FAIL 软链内容"; }
NL=$(find "$M" -type l | wc -l)
echo "软链数: $NL"
[ "$NL" -gt 40 ] || { echo "  FAIL: 软链数过少(留下的 applet 链接也没拷过去?)"; FAIL="$FAIL 软链数"; }
[ -e "$M/install.d" ] && { echo "  FAIL: 装好的盘里还有 install.d(会自触发重装)"; FAIL="$FAIL install.d"; }

echo ""
if [ -z "$FAIL" ]; then
    echo "check-installed-root: PASS (已安装的根完整可用)"
    exit 0
fi
echo "check-installed-root: FAIL ->$FAIL"
exit 1
