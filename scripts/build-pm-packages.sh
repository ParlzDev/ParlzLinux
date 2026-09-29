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
# 把实际版本留给 feed 用(包名里只有大版本号 13/18, 索引想写 13.3.0 得靠这个)。
# 注意 REPO 下面才 rm -rf, 这里先建目录再写。
mkdir -p "$REPO"
cat > "$REPO/VERSIONS" <<EOF
gcc=$GCC_FULL
clang=$CLANG_FULL
EOF

[ -d "$TC/opt/gcc-$GV" ] && [ -d "$TC/opt/llvm-$LV" ] || {
  echo "缺 $TC(先跑 scripts/build-toolchain.sh)"; exit 1; }

rm -rf "$REPO"
mkdir -p "$REPO"

# 打一个 pm 包: $1=包目录(含 opt/lib/usr/... 子树, 相对根布局)
# 产出成员名带 / 前缀(把根目录名替换成 /)。
pack_pm() {
  local root="$1" out="$2"
  # 成员名带 / 前缀(绝对路径)。
  # 做法: 在宿主真根 / 下建临时树 bind? 不可行(无 mount 权限)。
  # 实际方案: 把包内容树按 "opt/ lib/ usr/ bin/ ..." 相对布局打包,
  # cpio 成员名 = 相对名(如 opt/toolchain/...)。pm.c 安装时给相对名补 / 前缀,
  # 落根文件系统。相对名避免了 cpio 读宿主真实 /lib /usr 的歧义。
  ( cd "$root" && find . -mindepth 1 -print | sed 's|^\./||' | LC_ALL=C sort \
      | cpio -o -H newc 2>/dev/null ) > "$out"
  # 校验成员数非 0
  local n
  n=$(cpio -t < "$out" 2>/dev/null | grep -cv '^TRAILER' || true)
  [ "$n" -gt 0 ] || { echo "  !! $out 打空了"; return 1; }
  echo "    $(basename $out): 成员 $n, $(du -h "$out" | cut -f1)"
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
# binutils(as/ld/ar/ranlib/objdump/nm/strip): gcc 驱动按 /usr/bin/
# x86_64-linux-gnu-as 等找 binutils, guest 的 /usr/bin 下原本没有。
# 补单跳软链指包内 binutils, 裸 gcc ./a.c 不用 -B 就能命中 as/ld。
G15B="$G/opt/toolchain/gcc-$GV/bin"
for bu in as ld ar ranlib objdump nm strip; do
  [ -f "$G15B/x86_64-linux-gnu-$bu" ] && \
    ln -sfn "/opt/toolchain/gcc-$GV/bin/x86_64-linux-gnu-$bu" "$G/usr/bin/x86_64-linux-gnu-$bu"
  [ -f "$G15B/$bu" ] && \
    ln -sfn "/opt/toolchain/gcc-$GV/bin/$bu" "$G/usr/bin/$bu"
done
# 系统多架构路径软链树(驱动硬编码 /usr/lib/x86_64-linux-gnu/... 命中)
mkdir -p "$G/usr/lib/x86_64-linux-gnu"
for f in "$TC/lib/"*; do ln -sfn "/lib/toolchain/$(basename $f)" "$G/usr/lib/x86_64-linux-gnu/$(basename $f)" 2>/dev/null; done
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
# clang -l:libc.so.6 会走 gcc 驱动注入的 -lgcc_s(收集器硬编码)。
# 单独装 clang(未装 gcc 包)时宿主 gcc-$GV 无 libgcc_s.so, 从宿主
# /usr/lib/x86_64-linux-gnu 拷真实档案进 /lib/toolchain, 链接可命中。
cp -aL /usr/lib/x86_64-linux-gnu/libgcc_s.so "$L/lib/toolchain/libgcc_s.so.1" 2>/dev/null || true
ln -sfn /lib/toolchain/libgcc_s.so.1 "$L/lib/toolchain/libgcc_s.so" 2>/dev/null || true
# -lgcc 静态档案(收集器注入, 裸 clang 链接 C/C++ 时 ld 找 -lgcc):
# 宿主 gcc-$GV 私目录 lib/gcc/x86_64-linux-gnu/$GV/libgcc.a 拷进 clang 包
# /usr/lib(与 g++/clang 驱动默认搜索路径一致), 裸 clang 链接可命中。
mkdir -p "$L/usr/lib"
for glib in libgcc.a libgcc_eh.a; do
  [ -f "/usr/lib/gcc/x86_64-linux-gnu/$GV/$glib" ] && \
    cp -aL "/usr/lib/gcc/x86_64-linux-gnu/$GV/$glib" "$L/usr/lib/$glib" 2>/dev/null || true
done
# clang 多架构路径软链树(与 gcc 包同款, 让 clang 驱动硬编码
# /usr/lib/x86_64-linux-gnu 命中 glibc 共享库)
mkdir -p "$L/usr/lib/x86_64-linux-gnu"
for f in "$TC/lib/"*; do ln -sfn "/lib/toolchain/$(basename $f)" "$L/usr/lib/x86_64-linux-gnu/$(basename $f)" 2>/dev/null; done
# 版本化静态档案(C/C++ 链接需要, 宿主 /usr/lib 才有):
for mlib in libm-2.43.a libmvec.a; do
  [ -f "/usr/lib/x86_64-linux-gnu/$mlib" ] && \
    cp -aL "/usr/lib/x86_64-linux-gnu/$mlib" "$L/usr/lib/x86_64-linux-gnu/$mlib" 2>/dev/null || true
done
# gcc 私目录软链(clang 包独立装时供 C++ 链接 libgcc):
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
# clang 编译 C/C++ 也要系统头 + libgcc: 宿主 /usr/include 整树拷进 clang 包
# $L/usr/include, 解包后 clang 默认 -I/usr/include 命中; 同时 /usr/lib
# 放 gcc 私目录软链(指 gcc 包), 动态链接 -lgcc_s/-lgcc 可命中。
echo "    宿主 /usr/include 系统头补充(clang 包):"
mkdir -p "$L/usr"
if [ -d /usr/include ]; then
  cp -aL /usr/include/. "$L/usr/include." 2>/dev/null && \
    { mv "$L/usr/include." "$L/usr/include"; } || {
      echo "    WARNING: 宿主 /usr/include 拷贝失败, clang 包无系统头"; }
  echo "      已补充: $(find "$L/usr/include" 2>/dev/null | wc -l) 个文件/目录"
fi
# gcc 私目录软链(clang 驱动硬编码 /usr/lib/gcc/x86_64-linux-gnu/$GV 找 libgcc.a)
mkdir -p "$L/usr/lib/gcc/x86_64-linux-gnu"
ln -sf "/opt/toolchain/gcc-$GV/lib/gcc/x86_64-linux-gnu/$GV" \
  "$L/usr/lib/gcc/x86_64-linux-gnu/$GV" 2>/dev/null || true
# 多架构路径软链树(驱动硬编码 /usr/lib/x86_64-linux-gnu 命中 glibc 共享库)
mkdir -p "$L/usr/lib/x86_64-linux-gnu"
for f in "$TC/lib/"*; do ln -sfn "/lib/toolchain/$(basename $f)" "$L/usr/lib/x86_64-linux-gnu/$(basename $f)" 2>/dev/null; done
# clang 找 binutils: gcc 包已把 as/ld 软链到 /usr/bin, clang 驱动默认搜
# /usr/bin/x86_64-linux-gnu-as 等; 单独装 clang(未装 gcc 包)时宿主
# /usr/bin 若有系统 binutils 兜底, 无则提示装 gcc 包。
# 动态编译: clang 默认动态, 产物 NEEDED libstdc++.so.6/libc.so.6,
# 解释器 /lib64/ld-linux(包内), 运行时靠 LD_LIBRARY_PATH 命中 /lib/toolchain。
gen_manifest clang $CLANG_FULL gcc "Clang/LLVM 预编译工具链(x86_64, 含系统头 + 动态编译)" > "$L/pm.pkg"

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
      clang-*) nm=clang; ver=21.1.8;;
    esac
    printf '%s %s %s.pm %s\n' "$nm" "$ver" "$p" "$sz"
  done
} > "$REPO/Packages"
cat "$REPO/Packages"

echo "=== pm 包仓库就绪: $REPO ==="
ls -lh "$REPO"
echo "服务器端: 把 $REPO 挂静态 HTTP(S), guest /etc/pm/feeds.conf 写基址"
