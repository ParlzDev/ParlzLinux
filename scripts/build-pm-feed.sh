#!/bin/sh
# build-pm-feed.sh - 产出 pm 的 feed(镜像站根目录): Packages 索引 + 各 .pm 包。
#
# feed 默认落在 output/feed(PM_FEED_DIR 可改); pm-server.sh 把这个目录当站点根
# 托管即可, guest 里 /etc/pm/feeds.conf 或 PM_FEED 指过来。
#
# 包:
#   core.pm   默认系统里**裁掉的命令**(userland/pm-trim.list):
#                   被裁的 busybox applet 软链 + 被裁的 userland 真二进制。
#                   ★ 不含 /sbin/busybox 本体 —— 本体留在默认系统里(所以
#                   `busybox <命令>` 一直可用), 包只补"命令名";
#                   这样 `pm remove core` 能干净退回瘦身前状态,
#                   不会把 busybox 本体一起删掉。
#   pm.pm           PM 包管理器自身(pm --version = pm+1.1-RC+1), 给老系统自更新
#   gcc.pm/clang.pm 有工具链时收入(/home/jgzyes/pm-repo 下已打好的, 或
#                   build-pm-packages.sh 现打的)
#
# Packages 行格式(pm.c 的 find_pkg_in_base 只取第 1、3 列):
#   包名 版本 包文件名 大小bytes
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/build-pm-feed.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

P=/mnt/f/Linux/Parlz
US=/home/jgzyes/parlz-userland
ROOT=$US/root
STAGE=$US/trim-stage
FEED=${PM_FEED_DIR:-$P/output/feed}
REPO=${PM_REPO:-/home/jgzyes/pm-repo}      # build-pm-packages.sh 的工具链包产出地

[ -d "$ROOT" ] || { echo "缺 $ROOT(先跑 build-userland.sh)"; exit 1; }
mkdir -p "$FEED"

# --- 版本 ---
PMVER="pm+0.0-A+0-dev"; PMV_SHORT="0.0-A+0"
if [ -f "$P/.pm-release" ]; then
    . "$P/.pm-release"
    PMVER="${PM_VERSION_ID:-$PMVER}"
    PMV_SHORT="${PM_MAJOR:-0}.${PM_MINOR:-0}-${PM_TYPE:-A}+${PM_N:-0}"
fi
OSVER="0.1.0"; OSSTAGE="dev"
if [ -f "$P/.parlz-release" ]; then
    . "$P/.parlz-release"
    OSVER="${PARLZ_VERSION:-$OSVER}"; OSSTAGE="${PARLZ_STAGE:-$OSSTAGE}"
fi
echo "feed: $FEED   PM 版本: $PMVER   Parlz 版本: $OSVER($OSSTAGE)"

TLINKS=$ROOT/etc/pm/trimmed-links
TBINS=$ROOT/etc/pm/trimmed-binaries
[ -f "$TLINKS" ] || { echo "缺 $TLINKS —— rootfs 没按 pm-trim.list 裁剪过?"; exit 1; }

# ★ 默认源必须是官网（用户 2026-09-28 明确要求；三处一致：盘上 feeds.conf / init 的 PM_FEED /
#   pm.c 的 FALLBACK_BASES）。pm.pm 会把 feeds.conf 打进包里，装到老系统上会覆盖盘上那份 ——
#   这里先卡一道，免得又把开发机地址发出去（踩过：http://10.0.2.2:8765）。
FEEDCONF=$(head -1 "$ROOT/etc/pm/feeds.conf" 2>/dev/null | tr -d '\r\n')
case $FEEDCONF in
  *parlz.com/feed*) ;;
  *) echo "!! $ROOT/etc/pm/feeds.conf 不是官网源（当前: '$FEEDCONF'）—— 先修 rootfs 再打包"; exit 1 ;;
esac

W=$FEED/.stage; rm -rf "$W"

# ============================ core.pm ============================
mkdir -p "$W/core/bin" "$W/core/usr/bin" "$W/core/etc/pm"
NLINK=0
while read -r rel; do
    [ -n "$rel" ] || continue
    d=${rel%/*}; n=${rel##*/}
    mkdir -p "$W/core/$d"
    case $d in
        # 目标规则与 gen-busybox-links.sh 一致(bin 用相对, usr/bin 用绝对),
        # 更深一层(sbin 等)用 ../../.. 数太脆 → 一律绝对 /sbin/busybox
        bin) ln -sf ../sbin/busybox "$W/core/$d/$n" ;;
        *)   ln -sf /sbin/busybox "$W/core/$d/$n" ;;
    esac
    NLINK=$((NLINK + 1))
done < "$TLINKS"

NBIN=0
if [ -f "$TBINS" ]; then
    while read -r rel; do
        [ -n "$rel" ] || continue
        n=${rel##*/}
        if [ -f "$STAGE/$n" ]; then
            mkdir -p "$W/core/$(dirname "$rel")"
            cp -a "$STAGE/$n" "$W/core/$rel"
            NBIN=$((NBIN + 1))
        else
            echo "  !! $rel 在 trimmed-binaries 里但 $STAGE/$n 不在, 跳过"
        fi
    done < "$TBINS"
fi

{
    echo "package: core"
    echo "version: $OSVER-$OSSTAGE"
    echo "provides-links: $NLINK (被裁的 busybox applet 软链)"
    echo "provides-binaries: $NBIN (被裁的 userland 真二进制)"
    echo "note: 本体 /sbin/busybox 属于默认系统, 本包不带也不删它。"
    echo "list-links: $TLINKS"
    echo "list-binaries: $TBINS"
} > "$W/core/etc/pm/core.manifest"

( cd "$W/core" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc 2>/dev/null ) \
    > "$FEED/core.pm"
rm -rf "$W/core"
CORE_SIZE=$(wc -c < "$FEED/core.pm")
echo "  core.pm: 软链 $NLINK + 真二进制 $NBIN, $CORE_SIZE 字节"

# ============================== pm.pm ================================
mkdir -p "$W/pm/bin" "$W/pm/etc/pm"
if [ -f "$ROOT/bin/pm" ]; then
    cp -a "$ROOT/bin/pm" "$W/pm/bin/pm"
else
    echo "  !! 缺 $ROOT/bin/pm"; exit 1
fi
# ★ **不把 /etc/pm/feeds.conf 打进包里**(2026-09-28 改): 装包不得改写机器的源配置。
#   原来装 pm 会顺带覆盖盘上那份 —— 好处是"自更新把默认源带过去", 代价是**任何
#   一次旧包安装都会把源改回旧地址**(实测: 官网 feed 里那份 9-27 的 pm.pm 带着
#   开发机地址, `pm install pm` 之后 `pm install core` 就跑去连 10.0.2.2:8765)。
#   默认源由盘上 feeds.conf(交付烘焙) + pm.c 的 FALLBACK_BASES[0](官网) 保证。
echo "package: pm" > "$W/pm/etc/pm/pm.manifest"
echo "version: $PMVER" >> "$W/pm/etc/pm/pm.manifest"
( cd "$W/pm" && find . -mindepth 1 | LC_ALL=C sort | cpio -o -H newc 2>/dev/null ) \
    > "$FEED/pm.pm"
rm -rf "$W/pm"
PM_SIZE=$(wc -c < "$FEED/pm.pm")
# 包里**不该**再有 feeds.conf —— 有就是打包逻辑回退了, 直接拦下。
# 注意必须看 **cpio 成员名**, 不能 grep 原始字节: /bin/pm 二进制里本来就内嵌
# 着 "/etc/pm/feeds.conf" 这个路径字符串(那是它的配置文件名), grep 会假报警。
if cpio -it < "$FEED/pm.pm" 2>/dev/null | grep -q 'etc/pm/feeds\.conf'; then
    echo "!! pm.pm 里带上了 feeds.conf(装包会改写机器的源配置) —— 检查打包列表"; exit 1
fi
echo "  pm.pm: $PM_SIZE 字节(装它会换掉正在运行的 /bin/pm; pm 已改成先 unlink 再建, 不撞 ETXTBSY, 装完立刻生效于下次调用)"

# ========================= gcc.pm / clang.pm =========================
# 有工具链包就收进 feed, 并改名成通用包名(gcc.pm / clang.pm) —— 索引里的版本
# 列写实际版本, guest 里 `pm install gcc` 不用知道宿主 gcc 的具体号。
idx=""
grab() {   # grab <索引包名> <版本> <源文件>
    src=$3
    [ -f "$src" ] || return 0
    cp -f "$src" "$FEED/$1.pm"
    echo "  $1.pm <- $(basename "$src") ($(du -h "$FEED/$1.pm" | cut -f1))"
    idx="$idx$1 $2 $1.pm $(wc -c < "$FEED/$1.pm")
"
}
# 实际版本优先取 build-pm-packages.sh 写的 VERSIONS(包名里只有大版本号 13/18)
GCC_V="unknown"; CLANG_V="unknown"
if [ -f "$REPO/VERSIONS" ]; then
    . "$REPO/VERSIONS" 2>/dev/null
    [ -n "${gcc:-}" ] && GCC_V="$gcc"
    [ -n "${clang:-}" ] && CLANG_V="$clang"
fi
if [ -d "$REPO" ]; then
    for f in "$REPO/gcc.pm" "$REPO"/gcc-*.pm; do
        [ -f "$f" ] && { grab gcc "$GCC_V" "$f"; break; }
    done
    for f in "$REPO/clang.pm" "$REPO"/clang-*.pm; do
        [ -f "$f" ] && { grab clang "$CLANG_V" "$f"; break; }
    done
fi
if ! printf '%s' "$idx" | grep -q '^gcc '; then
    echo "  没有 gcc 包: 需先产工具链包"
    echo "    sh $P/scripts/build-toolchain.sh && sh $P/scripts/build-pm-packages.sh"
    echo "    (build-toolchain.sh 从宿主 /usr/lib/gcc/x86_64-linux-gnu/<版本> 与"
    echo "     /usr/lib/llvm-<版本> 取; 宿主没有对应版本就先 apt 装)"
fi
if ! printf '%s' "$idx" | grep -q '^clang '; then
    echo "  没有 clang 包: 同上(且宿主需装有 clang/llvm)"
fi

# ============================ Packages 索引 ===========================
{
    printf 'core %s-%s core.pm %s\n' "$OSVER" "$OSSTAGE" "$CORE_SIZE"
    printf 'pm %s pm.pm %s\n' "$PMV_SHORT" "$PM_SIZE"
    printf '%s' "$idx"
} > "$FEED/Packages"
rmdir "$W" 2>/dev/null

echo "  Packages:"
sed 's/^/    /' "$FEED/Packages"
echo "=== feed 就绪: $FEED ==="
echo "    起服务: PM_REPO=$FEED sh $P/scripts/pm-server.sh"
echo "    guest : echo $FEED 对应的 http://<host>:<port> > /etc/pm/feeds.conf; pm available"
