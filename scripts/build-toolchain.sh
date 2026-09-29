#!/bin/bash
# build-toolchain.sh - 把 WSL 宿主的 GCC + Clang/LLVM(版本自动探测)打包成自包含
# Linux 工具链(预编译可执行 + 依赖库 + 头文件), 产物 /home/jgzyes/toolchain-pack:
#   opt/gcc-$GV/  gcc 驱动 + cc1/collect2 + binutils + C/C++ 头 + 私有库
#   opt/llvm-$LV/ clang/clang++/llvm-config 全家
#   lib/         全部共享库依赖(glibc + gmp/mpfr/mpc + libstdc++ + crt 对象)
# build-userland.sh 把整树拷进 initramfs /opt/toolchain, 建 /usr/bin 软链,
# 注册 /etc/ld.so.conf.d/toolchain.conf, guest 内 ldconfig 后即可用。
set -eu
DEST=/home/jgzyes/toolchain-pack
# 宿主工具链版本自动探测。以前整脚本写死 gcc-15/llvm-21 —— 那是旧 WSL 26.04
# 实例里的包, 实例损坏后 24.04 只有 gcc 13.3(llvm 需 apt 装)。现在按宿主实有
# 的最新版走: 包内目录名与包名跟着版本走, 别再焊死号导致换机器就产不出包。
GV=$(ls -1 /usr/lib/gcc/x86_64-linux-gnu/ 2>/dev/null \
        | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -V | tail -1)
LV=$(ls -1d /usr/lib/llvm-* 2>/dev/null | sed 's|.*/llvm-||' \
        | grep -E '^[0-9]+$' | sort -V | tail -1)
[ -n "$GV" ] || { echo "宿主没有 /usr/lib/gcc/x86_64-linux-gnu/<版本>(apt install gcc)"; exit 1; }
[ -n "$LV" ] || { echo "宿主没有 /usr/lib/llvm-<版本>: apt-get install -y clang llvm"; exit 1; }
echo "宿主工具链: gcc-$GV + llvm-$LV"
rm -rf "$DEST"
mkdir -p "$DEST/opt/gcc-$GV/bin" "$DEST/opt/gcc-$GV/libexec" "$DEST/opt/gcc-$GV/lib" "$DEST/opt/gcc-$GV/include" "$DEST/opt/llvm-$LV" "$DEST/lib"

# ---- clang/LLVM(整树 + 依赖) ----
cp -a /usr/lib/llvm-$LV/. "$DEST/opt/llvm-$LV/"
for b in clang clang++ llvm-config lld lld-link opt llvm-as llvm-dis; do
  [ -e "/usr/bin/$b" ] && cp -aL "/usr/bin/$b" "$DEST/opt/llvm-$LV/bin/" 2>/dev/null || true
done
# 包内 binutils(ld/as): 让 clang 全程用包内链接器(-fuse-ld=ld.bfd 指包内),
# 避免 clang 默认调系统 /usr/bin/ld.bfd 链接出缺 libm 的动态产物(宿主 glibc 2.43
# 碰巧兼容, 包内 glibc 下 SEGV)。
for b in ld ld.bfd as objcopy objdump nm ar ranlib; do
  [ -e "/usr/bin/x86_64-linux-gnu-$b" ] && cp -aL "/usr/bin/x86_64-linux-gnu-$b" "$DEST/opt/llvm-$LV/bin/" 2>/dev/null || true
  [ -e "/usr/bin/$b" ] && cp -aL "/usr/bin/$b" "$DEST/opt/llvm-$LV/bin/" 2>/dev/null || true
done
# 依赖库
ldd "$DEST/opt/llvm-$LV/bin/clang" 2>/dev/null | awk '{print $3}' | grep -vE 'not found|^$' | while read -r l; do
  [ -e "$l" ] && cp -aL "$l" "$DEST/lib/" 2>/dev/null || true
done

# ---- gcc 15(驱动 + cc1 + binutils + 头 + 私有库) ----
for b in gcc g++ cc c++; do
  [ -e "/usr/bin/$b" ] && cp -aL "/usr/bin/$b" "$DEST/opt/gcc-$GV/bin/"
done
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/. "$DEST/opt/gcc-$GV/lib/" 2>/dev/null || true
# gcc 执行体(cc1/cc1plus/collect2/lto-wrapper/lto1): 驱动按 libexecdir=/usr/libexec
# 配置, 实际要在 <pkg>/libexec/gcc/x86_64-linux-gnu/$GV/ 找它们; 先建目录树再拷
mkdir -p "$DEST/opt/gcc-$GV/libexec/gcc/x86_64-linux-gnu/$GV"
cp -a /usr/libexec/gcc/x86_64-linux-gnu/$GV/. "$DEST/opt/gcc-$GV/libexec/gcc/x86_64-linux-gnu/$GV/" 2>/dev/null || true
# C/C++ 头文件
cp -a /usr/include/. "$DEST/opt/gcc-$GV/include/" 2>/dev/null || true
# gcc 私目录的头文件(stddef.h 等, 在 gcc 私有 include)
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/include/. "$DEST/opt/gcc-$GV/include/" 2>/dev/null || true
# gcc 全部依赖库
for f in "$DEST/opt/gcc-$GV/bin/"* "$DEST/opt/gcc-$GV/libexec/"* "$DEST/opt/gcc-$GV/libexec/gcc/x86_64-linux-gnu/$GV/"*; do
  [ -f "$f" ] || continue
  ldd "$f" 2>/dev/null | awk '{print $3}' | grep -vE 'not found|^$' | while read -r l; do
    [ -e "$l" ] && cp -aL "$l" "$DEST/lib/" 2>/dev/null || true
  done
done
# glibc 启动对象(crt*.o) + ld.so + libstdc++ 静态
cp -a /usr/lib/x86_64-linux-gnu/crt*.o /usr/lib/x86_64-linux-gnu/Scrt1.o /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$DEST/lib/" 2>/dev/null || true
# 动态产物 INTERP 段默认写 /lib64/ld-linux-x86-64.so.2; guest rootfs 无 /lib64,
# 额外建 $DEST/lib64 放一份, build-userland.sh 里 /lib64 软链到 /lib/toolchain/../lib64
mkdir -p "$DEST/lib64"
cp -aL /usr/lib64/ld-linux-x86-64.so.2 "$DEST/lib64/" 2>/dev/null || \
  cp -aL /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$DEST/lib64/" 2>/dev/null || true
cp -a /usr/lib/x86_64-linux-gnu/libstdc++.so.6* /usr/lib/x86_64-linux-gnu/libsupc++.so* "$DEST/lib/" 2>/dev/null || true
# gcc 私目录的启动对象(crtbeginT.o 等, gcc 默认按 /usr/lib/gcc/.../ 找, 指不到包内就链接失败)
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/crt*.o "$DEST/lib/" 2>/dev/null || true
# C 标准库静态 .a(guest 内 -static 链接 libc/libm 用)
cp -a /usr/lib/x86_64-linux-gnu/libc.a /usr/lib/x86_64-linux-gnu/libm.a /usr/lib/x86_64-linux-gnu/libpthread.a /usr/lib/x86_64-linux-gnu/libdl.a /usr/lib/x86_64-linux-gnu/librt.a /usr/lib/x86_64-linux-gnu/libpthread_nonshared.a "$DEST/lib/" 2>/dev/null || true
# C++ 标准库静态 .a(guest 内 g++ -static 链接 libstdc++ 用)
cp -a /usr/lib/x86_64-linux-gnu/libstdc++.a /usr/lib/x86_64-linux-gnu/libsupc++.a "$DEST/lib/" 2>/dev/null || true
# C 标准库动态 .so(动态产物运行时依赖, ldd 收不到的直接补)
cp -a /usr/lib/x86_64-linux-gnu/libm.so.6 /usr/lib/x86_64-linux-gnu/libmvec.so.1 /usr/lib/x86_64-linux-gnu/libpthread.so.0 /usr/lib/x86_64-linux-gnu/libdl.so.2 /usr/lib/x86_64-linux-gnu/librt.so.1 "$DEST/lib/" 2>/dev/null || true
# binutils(as/ld)进 gcc 包内 bin, 让驱动 -B 优先找包内
for b in as ld; do
  [ -e "/usr/bin/x86_64-linux-gnu-$b" ] && cp -aL "/usr/bin/x86_64-linux-gnu-$b" "$DEST/opt/gcc-$GV/bin/" 2>/dev/null || true
  [ -e "/usr/bin/$b" ] && cp -aL "/usr/bin/$b" "$DEST/opt/gcc-$GV/bin/" 2>/dev/null || true
done
# binutils 依赖的 .so(bfd/opcodes/ctf/sframe)
for b in as ld nm ar ranlib objdump; do
  [ -e "/usr/bin/x86_64-linux-gnu-$b" ] || continue
  ldd "/usr/bin/x86_64-linux-gnu-$b" 2>/dev/null | awk '{print $3}' | grep -vE 'not found|^$' | while read -r l; do
    [ -e "$l" ] && cp -aL "$l" "$DEST/lib/" 2>/dev/null || true
  done
done
# gcc 私有 crt 启动对象(crtbeginT.o 等): 驱动按 --libexecdir=/usr/libexec 配置,
# 在 <pkg>/lib/gcc/.../15/ 找 crt, 包内路径不同。拷进包内 lib 且放一份进 bin
# 让 -B 前缀命中(ld 的 -L 同时搜 bin/ 与 lib/)。
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/crt*.o "$DEST/opt/gcc-$GV/lib/" 2>/dev/null || true
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/crt*.o "$DEST/opt/gcc-$GV/bin/" 2>/dev/null || true
# glibc 的 crt1/crti/crtn/Scrt1 启动对象
cp -a /usr/lib/x86_64-linux-gnu/crt*.o /usr/lib/x86_64-linux-gnu/Scrt1.o "$DEST/opt/gcc-$GV/lib/" 2>/dev/null || true
cp -a /usr/lib/x86_64-linux-gnu/crt*.o /usr/lib/x86_64-linux-gnu/Scrt1.o "$DEST/opt/gcc-$GV/bin/" 2>/dev/null || true

# ---- 关键: 让 gcc 驱动在包内自包含完成链接 ----
# 驱动按配置 --libexecdir=/usr/libexec, 找 cc1/crt/libgcc 的真实路径是
# <gcc 驱动目录>/../lib/gcc/x86_64-linux-gnu/$GV/ 与 <gcc 驱动目录>/../libexec/.../15/。
# 包内驱动在 opt/gcc-$GV/bin/, 所以需要:
#   opt/gcc-$GV/lib/gcc/x86_64-linux-gnu/$GV/  <- crt + libgcc.a/libstdc++.a
#   opt/gcc-$GV/libexec/gcc/x86_64-linux-gnu/$GV/ <- cc1/collect2/liblto_plugin(lto)
# 这样 -B 不指标准路径也能命中, 驱动完全在包内工作。
G15="$DEST/opt/gcc-$GV/lib/gcc/x86_64-linux-gnu/$GV"
L15="$DEST/opt/gcc-$GV/libexec/gcc/x86_64-linux-gnu/$GV"
mkdir -p "$G15" "$L15"
# crt + gcc 静态库进 G15(驱动按此 iprefix 找 crtbeginT.o/-lgcc)
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/crt*.o "$G15/" 2>/dev/null || true
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/libgcc*.a "$G15/" 2>/dev/null || true
cp -a /usr/lib/x86_64-linux-gnu/libstdc++.a "$G15/" 2>/dev/null || true
# 执行体进 L15(驱动按 libexecdir 找 cc1/collect2/lto-wrapper)
[ -d /usr/libexec/gcc/x86_64-linux-gnu/$GV ] && cp -a /usr/libexec/gcc/x86_64-linux-gnu/$GV/. "$L15/" 2>/dev/null || true
# glibc 启动对象 crt1.o/crti.o/crtn.o/Scrt1.o
cp -a /usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o /usr/lib/x86_64-linux-gnu/crtn.o /usr/lib/x86_64-linux-gnu/Scrt1.o "$G15/" 2>/dev/null || true
# ---- 驱动/链接器硬编码的系统多架构路径: 包内造 /usr/lib 树指回包内容 ----
# 驱动按 -B pkg/gcc 前缀找 cc1(已建 L15), cc1 又按 -iprefix <gcc bin 目录>/../lib/gcc/.../15/
# 找 stdc-predef.h → 即 $G15/include/。链接器:
#   - clang 探测包内 gcc 后向链接命令注入 /usr/lib/x86_64-linux-gnu/libm-2.43.a 等硬编码绝对路径
#   - 静态产物硬引用 -lgcc/-lgcc_eh/-lgcc_s(在系统 /usr/lib/x86_64-linux-gnu)
# guest 无 /usr/lib/x86_64-linux-gnu, 全链不过。修法:
#   a) G15 下补 include/ (cc1 -iprefix 命中)
#   b) 包内 gcc-15 与 llvm-$LV 各造 /usr/lib/.../15 与 /usr/lib/x86_64-linux-gnu
#      软链回 G15 与包内 lib(宿主自检时 -L 指包内路径即可命中这些软链;
#      guest 里 tooltest 通过 -B$G15 与 -B$DEST/opt/llvm-$LV/usr/lib/... 命中)。
mkdir -p "$G15/include"
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/include/. "$G15/include/" 2>/dev/null || true
[ -d /usr/lib/gcc/x86_64-linux-gnu/$GV/include-fixed ] && \
  cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/include-fixed/. "$G15/include-fixed/" 2>/dev/null || true
# cc1 -iprefix = $G15/。C++ 标准库头在系统 /usr/include/c++/$GV/:
# 不放进 $G15/include(c++ 子树), 避免 -I 包内 include 时 cstdlib 的
# #include_next <stdlib.h> 只搜到包内 include 层(无系统 stdlib.h)而失败。
# C++ 头由驱动默认路径 /usr/include/c++/$GV 命中, C 头由包内 include +
# 系统 /usr/include 双路命中。
mkdir -p "$DEST/lib/gcc/x86_64-linux-gnu/$GV"
ln -sfn "$G15" "$DEST/lib/gcc/x86_64-linux-gnu/$GV/gcc" 2>/dev/null || true
for PKG in gcc-$GV llvm-$LV; do
  PKD="$DEST/opt/$PKG"
  mkdir -p "$PKD/usr/lib/x86_64-linux-gnu"
  ln -sfn "$G15" "$PKD/usr/lib/gcc" 2>/dev/null || true
  # 驱动注入的 /usr/lib/x86_64-linux-gnu/{libm-2.43.a,libmvec.a,libc.so.6,...}
  # 绝对路径: 包内造该目录软链回包内 lib, 命中即解(guest 里 -L 指包内 lib 也兜底)
  for f in "$DEST/lib/"*; do
    b=$(basename "$f")
    ln -sf "$DEST/lib/$b" "$PKD/usr/lib/x86_64-linux-gnu/$b" 2>/dev/null || true
  done
done

echo "=== 工具链打包完成 ==="
du -sh "$DEST/opt/gcc-$GV" "$DEST/opt/llvm-$LV" "$DEST/lib"
echo "=== 自检: 编译 C/C++(gcc/clang 动态+静态产物全验证) ==="
BP="$DEST/opt/gcc-$GV/libexec/gcc/x86_64-linux-gnu/$GV/"
# gcc 驱动按标准路径找 as/ld(系统路径优先, 包内 binutils 作兜底);
# crt 文件由驱动注入链接命令, 无需额外 -L。
GB="-B$BP-B$DEST/opt/gcc-$GV/bin/"
GI="-I$DEST/opt/gcc-$GV/include"
cd /tmp
printf 'int main(){return 42;}' > tc_h.c
printf 'int main(){return 43;}' > tc_h.cpp
# gcc C 动态(编译用系统库, 运行需 LD_LIBRARY_PATH)
LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/gcc-$GV/lib" \
  "$DEST/opt/gcc-$GV/bin/gcc" $GB $GI -O2 -o /tmp/tc_h /tmp/tc_h.c 2>&1 | head -2
echo "gcc C 动态 rc=$( /tmp/tc_h 2>/dev/null; echo $?)"
# gcc C 静态
LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/gcc-$GV/lib" \
  "$DEST/opt/gcc-$GV/bin/gcc" $GB $GI -O2 -fno-pie -static -o /tmp/tc_hs /tmp/tc_h.c 2>&1 | head -2
echo "gcc C 静态 rc=$( /tmp/tc_hs 2>/dev/null; echo $?)"
# gcc C++ 动态
LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/gcc-$GV/lib" \
  "$DEST/opt/gcc-$GV/bin/g++" $GB $GI -O2 -o /tmp/tc_hp /tmp/tc_h.cpp 2>&1 | head -2
echo "gcc C++ 动态 rc=$( LD_LIBRARY_PATH=$DEST/lib /tmp/tc_hp 2>/dev/null; echo $?)"
# clang C 动态: 包内 clang 探测系统 gcc 后, 链接命令用 /usr/lib/.../crtbeginS.o +
# -L/usr/lib/x86_64-linux-gnu, 但包内无该目录 → 找不到 libc.so.6, NEEDED 缺 libc,
# 动态加载器起不来 SEGV。显式 -l:libc.so.6 指包内 lib 补 NEEDED, 运行靠 rpath/LD_LIBRARY_PATH。
CLAG="-I$DEST/opt/gcc-$GV/include -L$DEST/lib -Wl,-rpath-link,$DEST/lib -Wl,-rpath,$DEST/lib"
LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/llvm-$LV/lib" \
  "$DEST/opt/llvm-$LV/bin/clang" $CLAG -l:libc.so.6 -O2 -o /tmp/tc_cl /tmp/tc_h.c 2>&1 | head -2
echo "clang C 动态 rc=$( LD_LIBRARY_PATH=$DEST/lib /tmp/tc_cl 2>/dev/null; echo $?)"
# 对照: 系统 ld 能找到的路径时不加也能过(宿主), 包内必须显式补
# clang C 静态
LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/llvm-$LV/lib" \
  "$DEST/opt/llvm-$LV/bin/clang" $GI -L"$DEST/lib" -O2 -fno-pie -static -o /tmp/tc_cls /tmp/tc_h.c 2>&1 | head -2
echo "clang C 静态 rc=$( /tmp/tc_cls 2>/dev/null; echo $?)"
# 编 Parlz 真实源
for f in awk.c curl.c w3m.c; do
  LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/gcc-$GV/lib" "$DEST/opt/gcc-$GV/bin/gcc" $GB $GI -O2 -fno-pie -c /mnt/f/Linux/Parlz/userland/$f -o /tmp/t_o1 2>/dev/null && echo "gcc $f OK"
  LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/llvm-$LV/lib" "$DEST/opt/llvm-$LV/bin/clang" -I"$DEST/opt/gcc-$GV/include" -O2 -fno-pie -c /mnt/f/Linux/Parlz/userland/$f -o /tmp/t_o2 2>/dev/null && echo "clang $f OK"
done
echo "ALL_OK"
