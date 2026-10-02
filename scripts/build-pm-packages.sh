#!/bin/bash
# build-pm-packages.sh - 把工具链打成 pm 包(.pm = newc cpio + manifest),
# 产出 /home/jgzyes/pm-repo/(服务器端 feed 目录)。
#
# 产出结构:
#   /home/jgzyes/pm-repo/
#     Packages              # 包索引(行: 包名 版本 描述 包路径 大小)
#     gcc-$GV.pm          # gcc 驱动+cc1+binutils+头+静态库+共享库+ld.so
#     clang-llvm-$LV.pm   # clang/llvm 全家
#
# pm install gcc   → 下载 gcc-$GV.pm, 解包到 / (含 /opt/toolchain/gcc-$GV +
#                    /lib/toolchain + /lib64/ld.so + /usr/bin 软链 + manifest)
# pm install clang → 下载 clang-llvm-$LV.pm, 解包到 / (含 /opt/toolchain/llvm-$LV
#                    + 共享库 + /usr/bin 软链 + manifest)
#
# 包内容约定(pm.c 解包规则): 成员必须绝对路径(/...); 软链 = newc 0xA000 成员;
# 目录建 0755; 普通文件保留 mode; 跳过 TRAILER!。
#
# 服务器端: 把 /home/jgzyes/pm-repo/ 整目录放到 HTTP/HTTPS 静态站点
# (任意 web server/nginx/python http.server), guest 里配
# /etc/pm/feeds.conf = http(s)://<host>/pm-repo
# 包成员名: 带 / 前缀的绝对路径(/opt/... /usr/... 等), pm 安装时直接落 /
# 下(不走相对路径, 解包无 cwd 依赖)。打包用 find /opt 的绝对名喂 cpio。
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

TC=/home/jgzyes/toolchain-pack
REPO=/home/jgzyes/pm-repo
WORK=/home/jgzyes/pm-work

# 版本从 toolchain-pack 里读(build-toolchain.sh 按宿主实有的 gcc/llvm 建目录),
# 包名、manifest 版本列、Packages 索引全用它 —— 不再写死 15.2 / 21.1,
# 否则换一台宿主(比如 24.04 的 gcc 13.3 + clang 18)就产不出包。
GV=$(ls -1 "$TC/opt" 2>/dev/null | sed -n 's|^gcc-||p' | sort -V | tail -1)
LV=$(ls -1 "$TC/opt" 2>/dev/null | sed -n 's|^llvm-||p' | sort -V | tail -1)
[ -d "$TC/opt/gcc-$GV" ] && [ -d "$TC/opt/llvm-$LV" ] || {
  echo "缺 $TC/opt/gcc-<版本> 与 llvm-<版本>(先跑 scripts/build-toolchain.sh, 它自己探测宿主版本)"; exit 1; }
# -dumpversion 在 Ubuntu 的 gcc 上只给 "13"; -dumpfullversion 才给 13.3.0
GCC_FULL=$("$TC/opt/gcc-$GV/bin/gcc" -dumpfullversion 2>/dev/null \
           || "$TC/opt/gcc-$GV/bin/gcc" -dumpversion 2>/dev/null || echo "$GV")
CLANG_FULL=$("$TC/opt/llvm-$LV/bin/clang" --version 2>/dev/null \
             | sed -n 's/.*version \([0-9.]*\).*/\1/p' | head -1)
if [ -z "$CLANG_FULL" ]; then CLANG_FULL="$LV"; fi
echo "打包目标: gcc $GCC_FULL -> gcc-$GV.pm, clang $CLANG_FULL -> clang-llvm-$LV.pm"

[ -d "$TC/opt/gcc-$GV" ] && [ -d "$TC/opt/llvm-$LV" ] || {
  echo "缺 $TC(先跑 scripts/build-toolchain.sh)"; exit 1; }

rm -rf "$REPO"
mkdir -p "$REPO"
# 把实际版本留给 feed 用(包名里只有大版本号 13/18, feed 的索引想写 13.3.0
# 就得靠这份)。★ 必须写在 rm -rf "$REPO" **之后** —— 以前写在前面, 下面那句
# 重建仓库目录顺手把它删了, 于是 output/feed/Packages 里 gcc/clang 的版本列
# 一直是 "unknown"(feed 读不到 VERSIONS 就填这个), guest `pm available` 也跟着
# 显示 unknown。注释当时还写着"注意 REPO 下面才 rm -rf, 这里先建目录再写" ——
# 说了等于没做。
cat > "$REPO/VERSIONS" <<EOF
gcc=$GCC_FULL
clang=$CLANG_FULL
EOF

# 打一个 pm 包: $1=包目录(含 opt/lib/usr/... 子树, 相对根布局)
# 产出成员名带 / 前缀(把根目录名替换成 /)。
pack_pm() {
  local root="$1" out="$2"
  # 成员名带 / 前缀(绝对路径)。
  # 做法: 在宿主真根 / 下建临时树 bind? 不可行(无 mount 权限)。
  # 实际方案: 把包内容树按 "opt/ lib/ usr/ bin/ ..." 相对布局打包,
  # cpio 成员名 = 相对名(如 opt/toolchain/...)。pm.c 安装时给相对名补 / 前缀,
  # 落根文件系统。相对名避免了 cpio 读宿主真实 /lib /usr 的歧义。
  local srcn cerr err n
  srcn=$( cd "$root" && find . -mindepth 1 | wc -l )
  cerr=$(mktemp)
  # ★ cpio -o 的 stderr 以前是 `2>/dev/null` 直接丢掉的 —— "Cannot stat"/
  #   "Premature end" 这类真报错也跟着被吞, 包少档案也不报。改成收下来判
  #   (`cpio:` 前缀才算错, 成功时那句 "N blocks" 也在 stderr 上)。
  ( cd "$root" && find . -mindepth 1 -print | sed 's|^\./||' | LC_ALL=C sort \
      | cpio -o -H newc ) > "$out" 2>"$cerr"
  if grep -q 'cpio:' "$cerr"; then
    echo "  !! 打包报错: $(grep -m1 'cpio:' "$cerr")"
    rm -f "$cerr"; return 1
  fi
  rm -f "$cerr"
  # ★ 写完必须 sync 再复核: 1.5 GB 一次写下去, 数据全在页缓存里。实测踩过
  #   WSL 实例在打包结束后被重启, pm-repo 里 gcc-13.pm 只剩 60 MiB、
  #   Packages 变 0 字节 —— 而脚本当时已经打印过"546M", 完全看不出被截断。
  sync
  # 校验一: cpio 的**判据只能看 stderr + 成员数**。被截断的归档 cpio -t 仍然
  # 返回 0, 只在 stderr 打 "premature end of file"(实测), 光看退出码会被骗。
  # 校验二: 成员数与源树一致(丢档案/半途而废都出得来)。
  # ★ 注意 GNU cpio -t **不打印 TRAILER!!! 这一条**(以前那句
  #   `grep -cv '^TRAILER'` 因此是个空操作), 而它在**成功**时会往 stderr 打
  #   "N blocks" —— 所以 stderr 判错只能认 `cpio:` 前缀, 认"非空"会把好包判死。
  local err n
  err=$(mktemp)
  cpio -t < "$out" >"$err.list" 2>"$err"
  if grep -q 'cpio:' "$err"; then
    echo "  !! $out 归档不完整: $(grep -m1 'cpio:' "$err")"
    rm -f "$err" "$err.list"; return 1
  fi
  n=$(wc -l < "$err.list")
  rm -f "$err" "$err.list"
  [ "$n" -gt 0 ] || { echo "  !! $out 打空了"; return 1; }
  if [ "$n" != "$srcn" ]; then
    echo "  !! $out 成员数与源树不符(源 $srcn, 包 $n)—— cpio 被截断或有档案没进包"
    return 1
  fi
  echo "    $(basename "$out"): 成员 $n, $(du -h "$out" | cut -f1)"
  stat -c '    字节 %s' "$out"
}

# 生成 manifest(包名/版本/依赖/摘要)
gen_manifest() {
  # $1=包名 $2=版本 $3=依赖(空格分隔,可空) $4=摘要
  cat <<EOF
pkg: $1
version: $2
depends: $3
arch: x86_64-linux-gnu
summary: $4
EOF
}

# ---- 悬空软链闸门 ----
# 包里的软链目标是 **guest 路径**(/lib/toolchain/... /opt/toolchain/...),
# 所以在宿主上 `test -e` 一律"存在"(宿主的 /lib/toolchain 是另一个东西),
# 测不出断链。这里按 guest 视角解析: 把目标拼到包根下再判存在。
# 为什么值得挡: g++ 那条链就是目标文件不存在却照样打进了包, 装到 guest 里
# 表现为 "g++: not found"(实测), 现场看像"包没装上"。
no_dangle() { # $1=包根 $2..=要检查的相对目录
  local root="$1"; shift
  python3 - "$root" "$@" <<'PY'
import os, sys
root = sys.argv[1]
subdirs = sys.argv[2:]
bad = []
for sd in subdirs:
    base = os.path.join(root, sd)
    if not os.path.isdir(base):
        continue
    for dp, dn, fn in os.walk(base):
        for n in fn + dn:
            p = os.path.join(dp, n)
            if not os.path.islink(p):
                continue
            t = os.readlink(p)
            q = t if os.path.isabs(t) else os.path.normpath(os.path.join(dp[len(root):], t))
            if not os.path.exists(os.path.join(root, q.lstrip('/'))):
                bad.append(p[len(root):] + " -> " + t)
if bad:
    print("  !! 包内悬空软链(guest 里就是 not found):")
    for b in sorted(bad)[:25]:
        print("     ", b)
    sys.exit(1)
PY
}

# ---- gcc 包 ----
echo ">>> [1/2] 打 gcc-$GV.pm (gcc/g++/cc/c++ + binutils + 系统头 + 静态/共享库 + ld.so)"
G="$WORK/gcc-root"
rm -rf "$G"; mkdir -p "$G"
# 拷整树(绝对路径布局: 解包时直接落 /)
mkdir -p "$G/opt/toolchain"
cp -a "$TC/opt/gcc-$GV" "$G/opt/toolchain/gcc-$GV"
mkdir -p "$G/lib/toolchain"
cp -a "$TC/lib/." "$G/lib/toolchain/"
[ -f "$TC/lib64/ld-linux-x86-64.so.2" ] && mkdir -p "$G/lib64" && cp -a "$TC/lib64/ld-linux-x86-64.so.2" "$G/lib64/"
[ -f "$TC/lib/ld-linux-x86-64.so.2" ] && mkdir -p "$G/lib64" && cp -a "$TC/lib/ld-linux-x86-64.so.2" "$G/lib64/" 2>/dev/null || true
# /usr/bin 软链(单跳绝对链指 /opt/toolchain, 解包一次命中)
mkdir -p "$G/usr/bin" "$G/bin"
ln -sfn /opt/toolchain/gcc-$GV/bin/gcc   "$G/usr/bin/gcc"
ln -sfn /opt/toolchain/gcc-$GV/bin/g++   "$G/usr/bin/g++"
ln -sfn /opt/toolchain/gcc-$GV/bin/cc    "$G/usr/bin/cc"
ln -sfn /opt/toolchain/gcc-$GV/bin/c++   "$G/usr/bin/c++"
# /bin 单跳指包内(与 build-userland 约定一致)
ln -sfn /opt/toolchain/gcc-$GV/bin/gcc   "$G/bin/gcc"
ln -sfn /opt/toolchain/gcc-$GV/bin/g++   "$G/bin/g++"
# binutils + 预处理器: gcc/clang 驱动按 /usr/bin/x86_64-linux-gnu-as 等找,
# guest 的 /usr/bin 下原本没有; 用户也要 readelf/objcopy 才敢说自己会动态编译
# (以前只链 as/ld/ar/ranlib/objdump/nm/strip, readelf 直接 not found)。
G15B="$G/opt/toolchain/gcc-$GV/bin"
for bu in as ld ld.bfd ar ranlib objdump nm strip readelf objcopy size strings cpp gcov gcc-ar gcc-ranlib; do
  [ -f "$G15B/x86_64-linux-gnu-$bu" ] && \
    ln -sfn "/opt/toolchain/gcc-$GV/bin/x86_64-linux-gnu-$bu" "$G/usr/bin/x86_64-linux-gnu-$bu"
  [ -f "$G15B/$bu" ] && ln -sfn "/opt/toolchain/gcc-$GV/bin/$bu" "$G/usr/bin/$bu"
done
# 系统多架构路径软链树(驱动与链接器脚本硬编码这两个目录的绝对路径;
# ld.so 无 /etc/ld.so.cache 时的默认搜索目录也是它们 —— 动态产物**不设
# LD_LIBRARY_PATH** 能不能起来就看这里)
mk_mtrees() { # $1=包根
  mkdir -p "$1/usr/lib/x86_64-linux-gnu" "$1/lib/x86_64-linux-gnu" "$1/lib"
  for f in "$TC/lib/"*; do
    b=$(basename "$f")
    # 目录不收: $TC/lib/gcc/... 是包内私树的入口, 链成 /usr/lib/gcc 会让
    # 后面 `mkdir -p usr/lib/gcc/x86_64-linux-gnu` 顺着软链把假目录写进
    # lib/toolchain/gcc/ 里(实测会污染包内树)。
    [ -d "$f" ] && continue
    ln -sfn "/lib/toolchain/$b" "$1/usr/lib/x86_64-linux-gnu/$b" 2>/dev/null
    ln -sfn "/lib/toolchain/$b" "$1/lib/x86_64-linux-gnu/$b"   2>/dev/null
    ln -sfn "/lib/toolchain/$b" "$1/lib/$b"                    2>/dev/null
  done
}
mk_mtrees "$G"
# gcc 私目录(驱动硬编码 /usr/lib/gcc/x86_64-linux-gnu/$GV -> 包内 G15):
# 裸 g++/gcc 动态链接时找 libgcc_s.so, 包内补软链指回 /opt/toolchain/gcc-$GV。
mkdir -p "$G/usr/lib/gcc/x86_64-linux-gnu"
ln -sf "/opt/toolchain/gcc-$GV/lib/gcc/x86_64-linux-gnu/$GV" \
  "$G/usr/lib/gcc/x86_64-linux-gnu/$GV" 2>/dev/null || true
# 系统 C 头文件: 包内 include(=宿主 glibc-2.43 的 include/ 副本)只有 glibc
# 部分, stdio.h/stdlib.h/string.h 等常见系统在 /usr/include, 包里没有。
# 宿主 /usr/include 整树(真实文件+子目录)拷进 $G/usr/include, 解包后 gcc
# 默认 -I/usr/include(见 gcc -v search list)直接命中; C++ 也 OK。
# 只拷宿主存在且可访问的项, 失败不阻断(宿主无某子目录时跳过)。
echo "    宿主 /usr/include 系统头补充:"
mkdir -p "$G/usr"
if [ -d /usr/include ]; then
  cp -aL /usr/include/. "$G/usr/include." 2>/dev/null && \
    { mv "$G/usr/include." "$G/usr/include"; } || {
      echo "    WARNING: 宿主 /usr/include 拷贝失败, gcc 包无系统头(裸 gcc 编 C 将缺 stdio.h)"; }
  echo "      已补充: $(find "$G/usr/include" 2>/dev/null | wc -l) 个文件/目录"
fi
# manifest + 包名(供 pm 识别)
gen_manifest gcc $GCC_FULL "" "GCC 预编译工具链(x86_64, 含 glibc + binutils + 系统头)" > "$G/pm.pkg"
no_dangle "$G" usr/bin bin lib lib64 usr/lib/x86_64-linux-gnu usr/lib/gcc \
                 opt/toolchain/gcc-$GV/usr/lib/x86_64-linux-gnu \
                 opt/toolchain/gcc-$GV/usr/lib/gcc \
                 opt/toolchain/gcc-$GV/lib/gcc

pack_pm "$G" "$REPO/gcc-$GV.pm"
echo "    gcc-$GV.pm: $(du -h $REPO/gcc-$GV.pm | cut -f1)"

# ---- clang/llvm 包 ----
echo ">>> [2/2] 打 clang-llvm-$LV.pm (clang/clang++/llvm-config + LLVM 运行时)"
L="$WORK/llvm-root"
rm -rf "$L"; mkdir -p "$L"
mkdir -p "$L/opt/toolchain"
cp -a "$TC/opt/llvm-$LV" "$L/opt/toolchain/llvm-$LV"
# clang 的动态依赖(共享库)也归进 /lib/toolchain(与 gcc 包共享, 装 gcc 先则已有;
# 单独装 clang 时此包也带上所需 .so)
mkdir -p "$L/lib/toolchain"
cp -a "$TC/lib/." "$L/lib/toolchain/" 2>/dev/null || true
[ -f "$TC/lib64/ld-linux-x86-64.so.2" ] && mkdir -p "$L/lib64" && cp -a "$TC/lib64/ld-linux-x86-64.so.2" "$L/lib64/"
# 多架构软链树 + /lib(与 gcc 包同款): 驱动与 glibc 链接脚本硬编码的绝对路径,
# 以及 ld.so 无 cache 时的默认搜索目录, 都靠这里命中。
# ★ 以前这里另有一句 `cp -aL /usr/lib/x86_64-linux-gnu/libgcc_s.so
#   $L/lib/toolchain/libgcc_s.so.1` —— 把**链接器脚本**(内容是
#   GROUP ( libgcc_s.so.1 -lgcc ))当成 so.1 收了进来, 于是
#   libgcc_s.so -> libgcc_s.so.1 -> GROUP(libgcc_s.so.1) 自引用。
#   $TC/lib 里已经有真档案 libgcc_s.so.1 与脚本 libgcc_s.so, 不再单独处理。
mk_mtrees "$L"
# gcc 私目录软链(clang 驱动按 /usr/lib/gcc/x86_64-linux-gnu/$GV 找
# libgcc.a/libstdc++.so/crtbeginS.o): 指 gcc 包的落点, 单装 clang 时悬空,
# 由下面的 dangle 闸门报出来(manifest 里 clang 依赖 gcc 就是为这个)。
mkdir -p "$L/usr/lib/gcc/x86_64-linux-gnu"
ln -sf "/opt/toolchain/gcc-$GV/lib/gcc/x86_64-linux-gnu/$GV" \
  "$L/usr/lib/gcc/x86_64-linux-gnu/$GV" 2>/dev/null || true
mkdir -p "$L/usr/bin" "$L/bin"
ln -sfn /opt/toolchain/llvm-$LV/bin/clang      "$L/usr/bin/clang"
ln -sfn /opt/toolchain/llvm-$LV/bin/clang++    "$L/usr/bin/clang++"
ln -sfn /opt/toolchain/llvm-$LV/bin/llvm-config "$L/usr/bin/llvm-config"
ln -sfn /opt/toolchain/llvm-$LV/bin/clang      "$L/bin/clang"
ln -sfn /opt/toolchain/llvm-$LV/bin/clang++   "$L/bin/clang++"
ln -sfn /opt/toolchain/llvm-$LV/bin/llvm-config "$L/bin/llvm-config"
# clang 也要 binutils(as/ld)与 readelf: 它自己不带 ld, 驱动按
# /usr/bin/x86_64-linux-gnu-as 等找。包内 llvm-$LV/bin 里有这些(收集器拷过),
# 补单跳软链, 单装 clang 也能编。
LB="$L/opt/toolchain/llvm-$LV/bin"
for bu in as ld ld.bfd ar ranlib objdump nm strip readelf objcopy size strings; do
  [ -f "$LB/$bu" ] && ln -sfn "/opt/toolchain/llvm-$LV/bin/$bu" "$L/usr/bin/$bu"
  [ -f "$LB/x86_64-linux-gnu-$bu" ] && \
    ln -sfn "/opt/toolchain/llvm-$LV/bin/x86_64-linux-gnu-$bu" "$L/usr/bin/x86_64-linux-gnu-$bu"
done
# 系统头: 宿主 /usr/include 整树(cp -aL 解引用, 包里是真档案)
echo "    宿主 /usr/include 系统头补充(clang 包):"
mkdir -p "$L/usr"
if [ -d /usr/include ]; then
  cp -aL /usr/include/. "$L/usr/include." 2>/dev/null && \
    { mv "$L/usr/include." "$L/usr/include"; } || {
      echo "    WARNING: 宿主 /usr/include 拷贝失败, clang 包无系统头"; }
  echo "      已补充: $(find "$L/usr/include" 2>/dev/null | wc -l) 个文件/目录"
fi
# clang 找 binutils: gcc 包已把 as/ld 软链到 /usr/bin, clang 驱动默认搜
# /usr/bin/x86_64-linux-gnu-as 等; 上面本包也补了一份, 不再依赖 gcc 包在装。
# 动态编译: clang 默认动态, 产物 NEEDED libc.so.6/libstdc++.so.6,
# 解释器 /lib64/ld-linux-x86-64.so.2(包内), 运行时由 /usr/lib/x86_64-linux-gnu
# 与 /lib/x86_64-linux-gnu 两棵软链树命中 —— **不需要 LD_LIBRARY_PATH**。
gen_manifest clang $CLANG_FULL gcc "Clang/LLVM 预编译工具链(x86_64, 含系统头 + 动态编译)" > "$L/pm.pkg"
# 不查 usr/lib/gcc*: 那一支按设计指向 **gcc 包**的落点(manifest 里
# depends: gcc 就是这件事的声明), 单独看 clang 包必然"悬空"。
no_dangle "$L" usr/bin bin lib lib64 usr/lib/x86_64-linux-gnu \
                 opt/toolchain/llvm-$LV/usr/lib/x86_64-linux-gnu

pack_pm "$L" "$REPO/clang-llvm-$LV.pm"
echo "    clang-llvm-$LV.pm: $(du -h $REPO/clang-llvm-$LV.pm | cut -f1)"

# ---- Packages 索引 ----
{
  printf '# pm feed index\n'
  printf '# 行格式: 包名 版本 包文件 大小bytes\n'
  for p in gcc-$GV clang-llvm-$LV; do
    sz=$(stat -c%s "$REPO/$p.pm")
    case $p in
      gcc-*) nm=gcc; ver=$GCC_FULL;;
      # 版本列取实际探测值: 以前这里写死 21.1.8(旧 26.04 实例的 clang),
      # 于是索引说 21.1.8 而装进去的是 18 —— pm list/--version 与索引对不上。
      clang-*) nm=clang; ver=$CLANG_FULL;;
    esac
    printf '%s %s %s.pm %s\n' "$nm" "$ver" "$p" "$sz"
  done
} > "$REPO/Packages"
sync                     # 大档案写完就落盘(见 pack_pm 里那条 WSL 重启丢缓存的教训)
cat "$REPO/Packages"

echo "=== pm 包仓库就绪: $REPO ==="
ls -lh "$REPO"
echo "服务器端: 把 $REPO 挂静态 HTTP(S), guest /etc/pm/feeds.conf 写基址"
