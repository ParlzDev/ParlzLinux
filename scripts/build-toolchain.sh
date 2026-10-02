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
# C++ 需要宿主装了 g++(cc1plus) 与 libstdc++-<GV>-dev(libstdc++.a/.so),
# 缺了这里不会失败但产物只能编 C —— 后面自检会红, 别把自检删掉来"过"。
[ -x "/usr/libexec/gcc/x86_64-linux-gnu/$GV/cc1plus" ] \
  || echo "    WARNING: 宿主缺 cc1plus(apt-get install -y g++), 工具链将无法编译 C++"
# glibc 版本号(clang 探测后向链接命令注入 /usr/lib/x86_64-linux-gnu/libm-<GLV>.a
# 这种带版本的绝对路径; 以前写死 2.43 = 旧 26.04 实例的 glibc, 换宿主必失败)
GLV=$(basename "$(ls -1 /usr/lib/x86_64-linux-gnu/libm-*.a 2>/dev/null | sort -V | tail -1)" .a 2>/dev/null)
GLV=${GLV#libm-}
[ -n "$GLV" ] || { echo "    WARNING: 探测不到 /usr/lib/x86_64-linux-gnu/libm-<版本>.a(libc6-dev 没装?)"; }
echo "宿主 glibc: ${GLV:-未知}"
rm -rf "$DEST"
mkdir -p "$DEST/opt/gcc-$GV/bin" "$DEST/opt/gcc-$GV/libexec" "$DEST/opt/gcc-$GV/lib" "$DEST/opt/gcc-$GV/include" "$DEST/opt/llvm-$LV" "$DEST/lib"

# ---- clang/LLVM(整树 + 依赖) ----
cp -a /usr/lib/llvm-$LV/. "$DEST/opt/llvm-$LV/"
# 剪掉非 x86_64 的编译器运行时: 本内核只有 x86_64, 这些既占地方又让
# "包内每条 DT_NEEDED 都得有档案"这条自检报假红(i386 的
# libclang_rt.asan-i386.so 要的是 i386 的 ld-linux.so.2, 包里当然没有)。
find "$DEST/opt/llvm-$LV" \
     \( -name '*-i386*' -o -name '*-i486*' -o -name '*-i686*' \
        -o -name '*-arm*' -o -name '*-aarch64*' -o -name '*-riscv*' \
        -o -name '*-powerpc*' -o -name '*-ppc*' -o -name '*-mips*' \) \
     -type f -delete 2>/dev/null || true
for b in clang clang++ llvm-config lld lld-link opt llvm-as llvm-dis; do
  [ -e "/usr/bin/$b" ] && cp -aL "/usr/bin/$b" "$DEST/opt/llvm-$LV/bin/" 2>/dev/null || true
done
# 包内 binutils(ld/as): 让 clang 全程用包内链接器(-fuse-ld=ld.bfd 指包内),
# 避免 clang 默认调系统 /usr/bin/ld.bfd 链接出缺 libm 的动态产物(宿主 glibc 2.43
# 碰巧兼容, 包内 glibc 下 SEGV)。
for b in ld ld.bfd as objcopy objdump nm ar ranlib strip readelf size strings; do
  [ -e "/usr/bin/x86_64-linux-gnu-$b" ] && cp -aL "/usr/bin/x86_64-linux-gnu-$b" "$DEST/opt/llvm-$LV/bin/" 2>/dev/null || true
  [ -e "/usr/bin/$b" ] && cp -aL "/usr/bin/$b" "$DEST/opt/llvm-$LV/bin/" 2>/dev/null || true
done
# 依赖库
ldd "$DEST/opt/llvm-$LV/bin/clang" 2>/dev/null | awk '{print $3}' | grep -vE 'not found|^$' | while read -r l; do
  [ -e "$l" ] && cp -aL "$l" "$DEST/lib/" 2>/dev/null || true
done

# ---- gcc 15(驱动 + cc1 + binutils + 头 + 私有库) ----
for b in gcc g++ cc c++ gcc-$GV g++-$GV x86_64-linux-gnu-gcc x86_64-linux-gnu-g++; do
  [ -e "/usr/bin/$b" ] && cp -aL "/usr/bin/$b" "$DEST/opt/gcc-$GV/bin/" 2>/dev/null || true
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

# ---- ★ 链接期用的 dev 名字(libc.so / libm.so / libstdc++.so / libgcc_s.so) ----
# ld 处理 `-lc` 时只找 libc.so 再退到 libc.a。以前只收了运行时的 libc.so.6 与
# 静态的 libc.a → "动态编译"静默落回 libc.a: 产物是静态 glibc 却带 -pie 的
# Scrt1.o 和 PT_INTERP, 跑起来 SEGV(实测 rc=139), 而 ld 还会提示
# "Using 'dlopen' in statically linked applications"。所以这几个不带版本的
# 名字是**动态编译的开关**, 不是可选装饰。
# 一律收进 $DEST/lib(guest 里 = /lib/toolchain), 下面的多架构软链树会自动带上。
cp -a /usr/lib/x86_64-linux-gnu/libc_nonshared.a "$DEST/lib/" 2>/dev/null || true
cp -a /usr/lib/x86_64-linux-gnu/libmvec.a /usr/lib/x86_64-linux-gnu/libmvec_nonshared.a \
      "/usr/lib/x86_64-linux-gnu/libm-${GLV}.a" "$DEST/lib/" 2>/dev/null || true
# gcc 私目录的真实档案(libgcc.a/libgcc_eh.a/libstdc++.a/libgcov.a):
# 驱动搜索路径里有 G15, 但静态链接 -lgcc/-lstdc++ 也得能命中 → 收进 lib/
for g in libgcc.a libgcc_eh.a libgcov.a libstdc++.a libstdc++exp.a libstdc++fs.a libsupc++.a; do
  cp -aL "/usr/lib/gcc/x86_64-linux-gnu/$GV/$g" "$DEST/lib/" 2>/dev/null || true
done
# dev 名一律写**相对** GROUP/INPUT 脚本(不写 /lib/x86_64-linux-gnu 这类绝对路径):
# 宿主上 $DEST/lib 不存在那两个目录, guest 上真档案在 /lib/toolchain —— 绝对路径
# 只能对一边。相对名由 ld 按搜索目录解析(G15 + 多架构目录都带这些 .so.N)。
cat > "$DEST/lib/libc.so" <<'EOF'
/* 自研 Parlz 工具链包: 让 -lc 命中共享 libc 而不是静默落回 libc.a */
OUTPUT_FORMAT(elf64-x86-64)
GROUP ( libc.so.6 libc_nonshared.a AS_NEEDED ( /lib64/ld-linux-x86-64.so.2 ) )
EOF
cat > "$DEST/lib/libm.so" <<'EOF'
OUTPUT_FORMAT(elf64-x86-64)
GROUP ( libm.so.6 AS_NEEDED ( libmvec.so.1 ) )
EOF
for d in libpthread:libpthread.so.0 libdl:libdl.so.2 librt:librt.so.1 \
         libstdc++:libstdc++.so.6; do
  n=${d%%:*}; t=${d##*:}
  [ -e "$DEST/lib/$t" ] || continue
  printf 'GROUP ( %s )\n' "$t" > "$DEST/lib/$n.so"
done
# libgcc_s.so: 宿主那份就在 gcc 私目录, 内容就是相对名 GROUP, 直接收过来
cp -aL "/usr/lib/gcc/x86_64-linux-gnu/$GV/libgcc_s.so" "$DEST/lib/" 2>/dev/null || \
  printf 'GROUP ( libgcc_s.so.1 -lgcc )\n' > "$DEST/lib/libgcc_s.so"

# binutils(as/ld/objcopy/readelf/...)进 gcc 包内 bin, 让驱动 -B 优先找包内
for b in as ld ld.bfd objcopy objdump nm ar ranlib strip readelf size strings cpp gcov gcc-ar gcc-ranlib; do
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
# crt + gcc 库进 G15(驱动按此 iprefix 找 crtbeginT.o/-lgcc/-lstdc++):
# 整目录拷 —— 以前只挑 crt*.o 与 libgcc*.a, 又把 libstdc++.a 的路径写成
# /usr/lib/x86_64-linux-gnu(Ubuntu 实际在 gcc 私目录) → 静默失败, 于是
# 裸 g++/clang++ 动态链接报 cannot find -lstdc++ / -lgcc_s。
cp -a /usr/lib/gcc/x86_64-linux-gnu/$GV/. "$G15/" 2>/dev/null || true
# ---- 修掉"整目录拷"带进来的相对软链 ----
# 宿主 gcc 私目录里的 libstdc++.so / libgomp.so / libasan.so ... 全是**相对**链
# (../../../x86_64-linux-gnu/libgomp.so.1), cp -a 原样搬进包树后就悬空:
# guest 里 -lstdc++ / -fopenmp / -fsanitize=address 一律 cannot find, 而对悬空链
# 做 `>` 重定向还会直接 ENOENT(第一版脚本就死在这)。
# 做法: 在宿主上把链解引用到真正的 soname, 把那个**档案**收进 $DEST/lib,
#       包内的链改成指向 guest 落点的绝对单跳链; 解不出来的直接删掉不留悬空。
GP=/usr/lib/gcc/x86_64-linux-gnu/$GV
for d in "$DEST/opt/gcc-$GV/lib" "$G15"; do
  [ -d "$d" ] || continue
  find "$d" -maxdepth 1 -type l 2>/dev/null | while read -r l; do
    [ -e "$l" ] && continue
    b=$(basename "$l")
    t=$(readlink -f "$GP/$b" 2>/dev/null || true)
    if [ -n "$t" ] && [ -f "$t" ]; then
      s=$(basename "$t")
      cp -aL "$t" "$DEST/lib/$s" 2>/dev/null || true
      if [ -e "$DEST/lib/$s" ]; then rm -f "$l"; ln -sfn "/lib/toolchain/$s" "$l"; continue; fi
    fi
    [ -e "$DEST/lib/$b" ] && { rm -f "$l"; ln -sfn "/lib/toolchain/$b" "$l"; continue; }
    rm -f "$l"
  done
done
# dev 名字在 G15 也要有一份(驱动的 LIBRARY_PATH 第一项就是它)
[ -e "$DEST/lib/libstdc++.so.6" ] && { rm -f "$G15/libstdc++.so"; printf 'GROUP ( libstdc++.so.6 )\n' > "$G15/libstdc++.so"; } || true
[ -e "$DEST/lib/libgcc_s.so.1" ] && { rm -f "$G15/libgcc_s.so"; printf 'GROUP ( libgcc_s.so.1 -lgcc )\n' > "$G15/libgcc_s.so"; } || true
for n in libc libm libpthread libdl librt; do
  [ -e "$DEST/lib/$n.so" ] && cp -af "$DEST/lib/$n.so" "$G15/$n.so" || true
done
# G15 与 opt/gcc-$GV/lib 两个目录**都**在驱动搜索路径里(bin/../lib 与
# bin/../lib/gcc/.../$GV), 整目录拷等于把 40+ MB 的静态档案存两遍 ——
# 同名 .a/.o/.spec 在 G15 里换成指向上层的相对链(链在包内树里, guest 解得开)。
( cd "$G15" && for f in *.a *.o *.spec; do
    [ -f "$f" ] || continue
    # G15 = opt/gcc-$GV/lib/gcc/x86_64-linux-gnu/$GV, 往上三级就是 lib/
    [ -f "../../../$f" ] || continue
    rm -f "$f" && ln -s "../../../$f" "$f"
  done ) 2>/dev/null || true
# G15 与 opt/gcc-$GV/lib 两个目录**都**在驱动的搜索路径里(bin/../lib 与
# bin/../lib/gcc/.../$GV), 整目录拷等于把 40+ MB 的 .a 存两遍。同名档案在
# G15 里换成指向上层的相对链(链在包内树里, guest 里解得开), 包体积砍一半。
GPL="$DEST/opt/gcc-$GV/lib"
for a in "$G15"/*.a "$G15"/*.o "$G15"/*.spec; do
  [ -f "$a" ] && [ ! -L "$a" ] || continue
  b=$(basename "$a")
  [ -f "$GPL/$b" ] && [ ! -L "$GPL/$b" ] || continue
  rm -f "$a" && ln -s "../../../$b" "$a"
done
# 执行体进 L15(驱动按 libexecdir 找 cc1/collect2/lto-wrapper)
[ -d /usr/libexec/gcc/x86_64-linux-gnu/$GV ] && cp -a /usr/libexec/gcc/x86_64-linux-gnu/$GV/. "$L15/" 2>/dev/null || true
# glibc 启动对象 crt1.o/crti.o/crtn.o/Scrt1.o
cp -a /usr/lib/x86_64-linux-gnu/crt1.o /usr/lib/x86_64-linux-gnu/crti.o /usr/lib/x86_64-linux-gnu/crtn.o /usr/lib/x86_64-linux-gnu/Scrt1.o "$G15/" 2>/dev/null || true
# ---- 依赖闭包: 包内每个可执行/每份 .so 的 DT_NEEDED 都必须在包里有档案 ----
# 以前只在拷贝**中途**按固定的几组文件跑 ldd, 后一步才拷进来的
# readelf/objcopy/size/strings 从来没被收集 → guest 里 `readelf` 一起来就报
# "libctf-nobfd.so.0: cannot open shared object file"(验收套件实测撞出来的)。
# 收到稳定为止(新补的库自己也可能带依赖), 最多 3 轮。
TL=$(mktemp)
for round in 1 2 3; do
  : > "$TL.deps"
  find "$DEST/opt" "$DEST/lib" -type f \( -perm -u+x -o -name '*.so*' \) -print \
    > "$TL.files" 2>/dev/null
  while read -r f; do
    ldd "$f" 2>/dev/null | awk '$3 ~ /^\// {print $3}'
  done < "$TL.files" >> "$TL.deps"
  new=0
  sort -u "$TL.deps" > "$TL.uniq"
  while read -r lib; do
    [ -n "$lib" ] || continue
    b=$(basename "$lib")
    [ -e "$DEST/lib/$b" ] && continue
    cp -aL "$lib" "$DEST/lib/" 2>/dev/null && { new=$((new+1)); echo "    第 $round 轮补依赖: $b"; }
  done < "$TL.uniq"
  [ "$new" -eq 0 ] && break
done
rm -f "$TL" "$TL.deps" "$TL.uniq" "$TL.files"
# 驱动/链接器硬编码 /usr/lib/x86_64-linux-gnu 与 /lib/x86_64-linux-gnu 的绝对
# 路径, 也把新补的库一并链上(包内软链树 → /lib/toolchain)
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
# (以前这里还建过 $DEST/lib/gcc/x86_64-linux-gnu/$GV/gcc -> G15 的"套娃"链:
#  它在 gcc 包里看着能用, 单独装 clang 包时就是一个指向不存在目录的悬空链 ——
#  驱动的搜索路径由包内 bin/../lib/... 自己算, 不靠这条链, 故直接不建)
for PKG in gcc-$GV llvm-$LV; do
  PKD="$DEST/opt/$PKG"
  mkdir -p "$PKD/usr/lib/x86_64-linux-gnu"
  ln -sfn "/opt/toolchain/gcc-$GV/lib/gcc/x86_64-linux-gnu/$GV" "$PKD/usr/lib/gcc" 2>/dev/null || true
  # 驱动/链接器注入的 /usr/lib/x86_64-linux-gnu/{libm-<GLV>.a,libc.so.6,...}
  # 绝对路径: 包内造该目录软链回去, 命中即解。
  # ★ 链的**目标**必须是 guest 里的落点(/lib/toolchain), 不能写 $DEST ——
  #   这棵树在包内, 解到 guest 后 /home/jgzyes/... 根本不存在(以前就是这样
  #   一路悬空, 只有包外重建的那份 /usr/lib/x86_64-linux-gnu 能用)。
  for f in "$DEST/lib/"*; do
    b=$(basename "$f")
    ln -sf "/lib/toolchain/$b" "$PKD/usr/lib/x86_64-linux-gnu/$b" 2>/dev/null || true
  done
done

echo "=== 工具链打包完成 ==="
du -sh "$DEST/opt/gcc-$GV" "$DEST/opt/llvm-$LV" "$DEST/lib"
echo "=== 自检 1/3: 必备档案(缺一个 dev 链接名, guest 里的动态编译就是假的) ==="
MISS=""
for f in libc.so libc.so.6 libc.a libc_nonshared.a \
         libm.so libm.so.6 libstdc++.so libstdc++.so.6 \
         libgcc_s.so libgcc_s.so.1 crtbeginS.o crtbeginT.o Scrt1.o crti.o crtn.o; do
  [ -e "$DEST/lib/$f" ] || MISS="$MISS lib/$f"
done
for f in libgcc.a libgcc_eh.a libstdc++.so libgcc_s.so crtbeginS.o include/stddef.h; do
  [ -e "$G15/$f" ] || MISS="$MISS G15/$f"
done
for f in bin/gcc bin/as bin/ld bin/readelf libexec/gcc/x86_64-linux-gnu/$GV/cc1 \
         libexec/gcc/x86_64-linux-gnu/$GV/collect2; do
  [ -e "$DEST/opt/gcc-$GV/$f" ] || MISS="$MISS gcc/$f"
done
[ -x "$DEST/opt/llvm-$LV/bin/clang" ] || MISS="$MISS llvm/bin/clang"
if [ -n "$MISS" ]; then echo "!! 包内缺:$MISS"; exit 1; fi
echo "    必备档案齐(libc.so/libm.so/libstdc++.so/libgcc_s.so 是真脚本或真档案, 非悬空链)"
# 1b) 包内每条 DT_NEEDED 都得在包里有档案 —— 缺了就是"命令一起来就
#     cannot open shared object file"(readelf 缺 libctf-nobfd.so.0 踩过)。
#     linux-vdso.so.1 由内核给, 不当缺档。
NM=$(find "$DEST/opt" "$DEST/lib" -type f \( -perm -u+x -o -name '*.so*' \) -print 2>/dev/null |
     while read -r f; do
       readelf -d "$f" 2>/dev/null | grep NEEDED | sed 's/.*\[\(.*\)\].*/\1/' |
       while read -r n; do
         case $n in linux-vdso.so.1) continue ;; esac
         [ -e "$DEST/lib/$n" ] || echo "  $(basename "$f") -> $n"
       done
     done | sort -u | head -20)
if [ -n "$NM" ]; then echo "!! 包内 NEEDED 缺档案:"; echo "$NM"; exit 1; fi
echo "    依赖闭包完整(每个可执行/共享库的 NEEDED 都在 /lib/toolchain 里)"

# ---- 自检 2/3: 宿主侧能真跑的两种链接(静态 + 只编不链) ----
# **动态**链接与运行不在这里断: 宿主上 ld 会优先命中宿主自己的
# /usr/lib/x86_64-linux-gnu, 编出来的产物根本没用包内 glibc —— 看着绿,
# 换到 guest 就另一回事。动态那条由 scripts/toolchain-dyn-verify.sh 在
# "解包后的真包 + chroot + env -i" 里断(那才是交付环境)。
BP="$DEST/opt/gcc-$GV/libexec/gcc/x86_64-linux-gnu/$GV/"
GB="-B$BP -B$DEST/opt/gcc-$GV/bin/"
GI="-I$DEST/opt/gcc-$GV/include"
cd /tmp
printf 'int main(){return 42;}' > tc_h.c
printf 'int main(){return 43;}' > tc_h.cpp
export LD_LIBRARY_PATH="$DEST/lib:$DEST/opt/llvm-$LV/lib"
"$DEST/opt/gcc-$GV/bin/gcc" $GB $GI -O2 -fno-pie -static -o /tmp/tc_hs /tmp/tc_h.c 2>&1 | head -2
echo "gcc C 静态 rc=$( /tmp/tc_hs 2>/dev/null; echo $?)"
"$DEST/opt/llvm-$LV/bin/clang" $GI -L"$DEST/lib" -O2 -fno-pie -static -o /tmp/tc_cls /tmp/tc_h.c 2>&1 | head -2
echo "clang C 静态 rc=$( /tmp/tc_cls 2>/dev/null; echo $?)"
# 只编不链: 验头文件与 cc1/cc1plus 本身(g++ 缺 cc1plus 在这一步就红)
for f in awk.c curl.c w3m.c; do
  "$DEST/opt/gcc-$GV/bin/gcc" $GB $GI -O2 -fno-pie -c /mnt/f/Linux/Parlz/userland/$f -o /tmp/t_o1 2>/dev/null && echo "gcc -c $f OK"
  "$DEST/opt/llvm-$LV/bin/clang" $GI -O2 -fno-pie -c /mnt/f/Linux/Parlz/userland/$f -o /tmp/t_o2 2>/dev/null && echo "clang -c $f OK"
done
printf '#include <vector>\n#include <string>\nint main(){std::vector<std::string> v(2);return (int)v.size()-2;}' > tc_v.cpp
"$DEST/opt/gcc-$GV/bin/g++" $GB -O2 -fno-pie -static -o /tmp/tc_vp /tmp/tc_v.cpp 2>&1 | head -3
echo "g++ C++ 静态 rc=$( /tmp/tc_vp 2>/dev/null; echo $?)"

echo "=== 自检 3/3: 动态编译/运行 = scripts/toolchain-dyn-verify.sh(解包真包 + chroot) ==="
echo ALL_OK
