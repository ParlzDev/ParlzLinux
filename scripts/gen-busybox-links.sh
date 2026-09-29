#!/bin/sh
# gen-busybox-links.sh - 在 rootfs 里建 busybox applet 软链(/bin、/usr/bin、/sbin、/usr/sbin)。
# 用法: sh gen-busybox-links.sh <rootfs> <busybox_bin>
#
# 读 busybox 自带 applet 清单(busybox --list, 或 busybox 无参时的
# "Usage:" 段), 给每个 applet 在标准 FHS 目录建软链指 busybox 本体。
# 与 userland 已自带的命令(cat/cp/... 真二进制)冲突时: 保留 userland
# 真二进制, 不建软链(避免覆盖)。

ROOT="$1"
BB="$2"
[ -n "$ROOT" ] && [ -n "$BB" ] || { echo "用法: $0 <rootfs> <busybox>"; exit 1; }

# busybox applet 清单: 优先 --list(每行一个 applet 名, 无逗号);
# 无 --list 时退回无参输出 "Currently defined functions:" 的逗号清单
LIST=$( "$BB" --list 2>/dev/null | tr -d ' \t' )
[ -n "$LIST" ] || LIST=$( "$BB" 2>/dev/null | awk '
  /Currently defined functions:/{f=1;next}
  f&&/^[A-Za-z_0-9,\[\] ]*$/&&NF
    { gsub(/[^A-Za-z0-9_\[\] ]/,""); gsub(/,/," "); print }
' )

# FHS 各目录该放哪些 applet(busybox 官方 install.sh 的分类, 简化版)
BIN_DIR=$ROOT/bin
UB_DIR=$ROOT/usr/bin
SBIN_DIR=$ROOT/sbin
USBIN_DIR=$ROOT/usr/sbin
mkdir -p "$BIN_DIR" "$UB_DIR" "$SBIN_DIR" "$USBIN_DIR"

# 真实命令名快照(userland 自带工具, 非软链): 跨目录判定重名。
# 必须跨目录 —— PATH 是 /usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin,
# 而 userland 真命令都在 /bin, busybox applet 软链却大量落在 /usr/bin,
# 于是 /usr/bin/<applet> 会**盖住** /bin 里的真命令。
# 实测踩过: guest 里敲 install 命中的是 busybox 的 install(拷贝文件工具),
# 而不是我们的安装器 —— 用户"在系统内装盘"直接失败。
REAL=""
for _d in "$BIN_DIR" "$SBIN_DIR" "$UB_DIR" "$USBIN_DIR"; do
  [ -d "$_d" ] || continue
  for _f in "$_d"/*; do
    [ -f "$_f" ] && [ ! -L "$_f" ] || continue
    REAL="$REAL $(basename "$_f")"
  done
done

# pm 裁剪名单(userland/pm-trim.list): 命中的 applet **不建软链**。
# 保留 /sbin/busybox 本体, 所以 `busybox <命令>` 仍然可用, 启动脚本不受影响;
# 命令名本身要靠 `pm install core`(scripts/build-pm-feed.sh 用下面这份
# /etc/pm/trimmed-links 打包)。
TRIM_SRC=${PARLZ_TRIM_LIST:-/mnt/f/Linux/Parlz/userland/pm-trim.list}
TRIM=""
if [ -f "$TRIM_SRC" ]; then
    TRIM=$(grep -v '^#' "$TRIM_SRC" | tr -d ' \t' | grep -v '^$' | tr '\n' ' ')
fi
TRIMMED=""

# 名单成员判定用逐词字符串比较, 不用 `case *" $app "*`: 名单里有 `[` 和 `[[`
# (test 关键字 applet), 它们在 case 的模式里是**通配字符类**, 会直接语法错。
in_trim() {
    for t in $TRIM; do
        if [ "$t" = "$1" ]; then return 0; fi
    done
    return 1
}

# 每行一个 applet, 循环建软链: 目标是真文件(非软链)则保留(不覆盖),
# 是软链指 busybox 则跳过; 其余(软链指他处/不存在)都建 busybox 软链。
# mklink <applet> <dir> <相对目标> <dir 的相对名(记进裁剪清单用)>
mklink() {
  app="$1"; d="$2"; tgt="$3"; rel="$4"
  # 真文件(非软链)保留: userland 二进制(cat/cp/mount/install...)优先
  [ -L "$d/$app" ] || { [ -e "$d/$app" ] && return 0; }
  # 其它目录里已有同名真命令 → 不建 busybox 软链(否则按 PATH 会抢先命中)
  case " $REAL " in *" $app "*) return 0 ;; esac
  # 在裁剪名单里 → 不建链, 按"相对目录/名字"记下来交给打包
  if in_trim "$app"; then
      TRIMMED="$TRIMMED $rel/$app"
      return 0
  fi
  # 软链已指 busybox 则跳过
  if [ -L "$d/$app" ] && readlink "$d/$app" 2>/dev/null | grep -q busybox; then
    return 0
  fi
  ln -sf "$tgt" "$d/$app" 2>/dev/null
}

for app in $LIST; do
  # 分类: 系统管理/核心工具放 /bin; 应用类工具放 /usr/bin
  case $app in
    init|halt|reboot|shutdown|poweroff|login|telinit|swapon|swapoff\
    |blkid|fdisk|mount|umount|pivot_root|insmod|rmmod|depmod\
    |modprobe|mkswap|mke2fs|fsck|syslogd|klogd|watch\
    |crond|ip|nslookup|ping|ping6|tftp|telnet|nc|ncat|logger\
    |getty|switch_root|mknod|mkfifo|mktemp\
    |ln|mv|cp|rm|rmdir|mkdir|ls|cat|echo|test|sh|ash|vi|busybox\
    |dd|head|tail|grep|sed|awk|cut|tr|sort|uniq|wc|paste\
    |find|xargs|which|dirname|basename|readlink|realpath\
    |df|du|stat|free|ps|top|kill|killall|sleep|date|time\
    |tar|gzip|gunzip|zcat|bzip2|unzip|expr|cal|seq|yes\
    |id|whoami|umask|env|export|printf|read\
    |chmod|chown|chgrp|su|sudo|who|uname|hostname|nproc\
    |file|md5sum|sha1sum|sha256sum|clear|stty\
    |ipconfig|route|udhcpc|ifconfig|ifup|ifdown\
    |more|od|hexdump|strings|nl|touch|tput|tsort|split\
    |col|column|csplit|join|pr|bc|shred|truncate)
      # /bin 下软链用相对 ../sbin/busybox(cpio newc 按路径解包, 相对链稳)
      mklink "$app" "$BIN_DIR" "../sbin/busybox" "bin"
      ;;
    *)
      # /usr/bin 下软链用绝对 /sbin/busybox(避免 cpio 解包顺序竞态,
      # 相对链 ../sbin/busybox 从 /usr/bin 看是 /usr/sbin/busybox 不存在)
      mklink "$app" "$UB_DIR" "/sbin/busybox" "usr/bin"
      ;;
  esac
done

# /usr/bin/busybox 与 /bin/busybox 本体软链(兜底命令名)。
# 用显式 ln 而非 mklink: /usr/bin 的软链目标是绝对路径 /sbin/busybox,
# 而 mklink 的 /usr/bin 分支 tgt 也是 /sbin/busybox(正确), 但 /bin 分支
# 的 tgt ../sbin/busybox 在 /usr/bin 下解析错, 故这里直接 ln。
ln -sf /sbin/busybox "$UB_DIR/busybox" 2>/dev/null
ln -sf ../sbin/busybox "$BIN_DIR/busybox" 2>/dev/null

# 被裁掉的 applet 清单落进 rootfs: build-pm-feed.sh 照着它建 core.pm,
# 人也可以在 guest 里 cat /etc/pm/trimmed-links 看"哪些命令要 pm 装"。
mkdir -p "$ROOT/etc/pm"
for n in $TRIMMED; do echo "$n"; done | LC_ALL=C sort > "$ROOT/etc/pm/trimmed-links"

# 名单里有、但 busybox 根本没编进来的名字(既没建链也没记进清单)——列出来免得
# 以后奇怪"这条去哪了"。用 comm 比对, 不用 case *"$n"*: 名单里有 `[`、`[[`,
# 它们在 case 的模式里是**字符类**, 会语法错。
if [ -n "$TRIM" ]; then
    TL=/tmp/parlz-trim.$$.list; BL=/tmp/parlz-bb.$$.list
    printf '%s\n' $TRIM | LC_ALL=C sort -u > "$TL"
    printf '%s\n' $LIST | LC_ALL=C sort -u > "$BL"
    NOTBB=$(comm -23 "$TL" "$BL")
    rm -f "$TL" "$BL"
    if [ -n "$NOTBB" ]; then
        printf '%s\n' "$NOTBB" > "$ROOT/etc/pm/trimmed-absent"
        echo "名单里但 busybox 没编这个 applet($(printf '%s\n' "$NOTBB" | wc -l) 个): $(echo $NOTBB | cut -c1-150)..."
    fi
fi

echo "busybox applet 软链: /bin=$(ls $BIN_DIR 2>/dev/null | wc -l) /usr/bin=$(ls $UB_DIR 2>/dev/null | wc -l) /sbin=$(ls $SBIN_DIR 2>/dev/null | wc -l) /usr/sbin=$(ls $USBIN_DIR 2>/dev/null | wc -l)  裁掉=$(wc -l < "$ROOT/etc/pm/trimmed-links" 2>/dev/null || echo 0)"
echo "关键验证: /sbin/init /bin/ash /bin/mount /usr/bin/busybox"
for p in $ROOT/sbin/init $ROOT/bin/ash $ROOT/bin/mount $ROOT/usr/bin/busybox; do
  [ -e "$p" ] && echo "  OK $p" || echo "  缺 $p"
done
