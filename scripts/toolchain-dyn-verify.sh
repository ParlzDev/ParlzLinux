#!/bin/bash
# toolchain-dyn-verify.sh - 断"gcc/clang 动态编译真的能用"。
#
# 为什么要有这条脚本: 工具链包以前在宿主自检里是绿的, 装到 guest 里却是
#   gcc 动态产物 SEGV(rc=139)、g++ not found、clang 报 cannot find -lgcc_s、
#   clang++ 报 cannot find /usr/lib/x86_64-linux-gnu/libm-2.39.a。
# 根因是"包里只收了运行时的 .so.N 与静态 .a, 没收 -lc/-lstdc++/-lgcc_s 这些
# **不带版本的 dev 名字**" —— 宿主自检看不见, 因为宿主自己什么都有。
#
# 做法: 把**真正交付的两个 .pm 包**解进空目录当 guest 根(= pm install 的落盘
# 结果), chroot 进去用 `env -i`(没有 LD_LIBRARY_PATH/CPATH/LIBRARY_PATH, 等价
# 于一个干净登录 shell)裸敲 gcc/g++/clang/clang++。判据一律断**终态**:
# 产物真跑出预期字 + readelf 的 DT_NEEDED/PT_INTERP 对 + 命令里不带任何
# -l:libc.so.6 / -Wl,-rpath 之类的补丁。
set -uo pipefail

W=${TCV_ROOT:-/home/jgzyes/tcdyn-root}
REPO=${TCV_REPO:-/home/jgzyes/pm-repo}
P=0; F=0
ok(){ echo "PASS  $1"; P=$((P+1)); }
ng(){ echo "FAIL  $1"; if [ -n "${2:-}" ]; then echo "$2" | sed 's/^/      | /' | head -12; fi; F=$((F+1)); }

GCCPM=$(ls -1 "$REPO"/gcc-*.pm 2>/dev/null | head -1)
CLPM=$(ls -1 "$REPO"/clang-llvm-*.pm 2>/dev/null | head -1)
[ -n "$GCCPM" ] && [ -n "$CLPM" ] || { echo "缺 $REPO/gcc-*.pm 与 clang-llvm-*.pm(先跑 scripts/build-pm-packages.sh)"; exit 1; }

# --- 装配 guest 根(包没变则复用, 1.4 GB cpio 不白解) ---
STAMP="$GCCPM:$(stat -c%s%Y "$GCCPM")|$CLPM:$(stat -c%s%Y "$CLPM")"
if [ -f "$W/.tcdyn-stamp" ] && [ "$(cat "$W/.tcdyn-stamp" 2>/dev/null)" = "$STAMP" ] && [ -d "$W/lib/toolchain" ]; then
  echo "复用已装配的 guest 根: $W"
else
  echo "装配 guest 根: $W <- $(basename "$GCCPM") + $(basename "$CLPM")"
  rm -rf "$W"; mkdir -p "$W"
  # 解包判据只能看 stderr 里的 `cpio:` 行: 被截断的 cpio 归档解出来**退出码
  # 仍是 0**(实测), 只打一行 "premature end of file"; 而成功时 stderr 也可能
  # 有输出, 所以认前缀不认非空 —— 只看 rc 会拿半个根去判产品。
  for pkg in "$GCCPM" "$CLPM"; do
    ( cd "$W" && cpio -idmu < "$pkg" 2>/tmp/tcdyn-cpio.err >/dev/null )
    if grep -q 'cpio:' /tmp/tcdyn-cpio.err; then
      echo "解包 $(basename "$pkg") 失败/不完整: $(grep -m1 'cpio:' /tmp/tcdyn-cpio.err)"
      exit 1
    fi
  done
  echo "$STAMP" > "$W/.tcdyn-stamp"
fi
mkdir -p "$W/tmp" "$W/t" "$W/proc" "$W/sys" "$W/dev" "$W/etc"
# chroot 里要一个 sh 与 env。它们只依赖 libc, 起来靠的正是包内 glibc +
# ld.so 默认搜索目录 —— 所以先把"根没装配好"和"产品有问题"分开。
cp -aL /bin/dash "$W/bin/sh" 2>/dev/null || cp -aL /bin/sh "$W/bin/sh"
cp -aL /usr/bin/env "$W/tmp/env"
if ! chroot "$W" /tmp/env -i /bin/sh -c 'echo shok' 2>&1 | grep -q '^shok$'; then
  echo "!! 根没装配好: chroot 里连 /bin/sh 都起不来, 后面判据全部作废"
  chroot "$W" /tmp/env -i /bin/sh -c 'echo x' 2>&1 | sed 's/^/      | /' | head -5
  exit 1
fi
ok "根可用(chroot 里的 /bin/sh 仅靠包内 glibc + ld.so 默认目录起来)"

inch(){ chroot "$W" /tmp/env -i PATH=/usr/bin:/bin /bin/sh -c "$1" 2>&1; }
has(){ local d="$1" pat="$2"; shift 2; local o; o=$(inch "$*"); \
       if printf '%s\n' "$o" | grep -qF -- "$pat"; then ok "$d"; else ng "$d" "$o"; fi; }
hasnt(){ local d="$1" pat="$2"; shift 2; local o; o=$(inch "$*"); \
       if printf '%s\n' "$o" | grep -qF -- "$pat"; then ng "$d" "不该出现「$pat」:$o"; else ok "$d"; fi; }
rcis(){ local d="$1" want="$2"; shift 2; inch "$*" >/dev/null 2>&1; local r=$?; \
       if [ "$r" = "$want" ]; then ok "$d"; else ng "$d" "rc=$r 期望 $want"; fi; }

# --- 测试源 ---
cat > "$W/t/hello.c" <<'EOF'
#include <stdio.h>
#include <math.h>
#include <string.h>
int main(void){ char b[8]; strcpy(b,"dyn-ok"); printf("%s sqrt2=%.6f\n", b, sqrt(2.0)); return 0; }
EOF
cat > "$W/t/hello.cc" <<'EOF'
#include <iostream>
#include <vector>
#include <string>
int main(){ std::vector<std::string> v={"cxx","dyn-ok"}; std::cout << v[0] << "-" << v[1] << std::endl; return 0; }
EOF
cat > "$W/t/libfoo.c" <<'EOF'
int foo(void); int foo(void){ return 42; }
EOF
cat > "$W/t/uselib.c" <<'EOF'
#include <stdio.h>
extern int foo(void);
int main(void){ printf("libfoo=%d\n", foo()); return 0; }
EOF
cat > "$W/t/usedl.c" <<'EOF'
#include <stdio.h>
#include <dlfcn.h>
int main(void){
  void *h = dlopen("libfoo.so.1", RTLD_NOW);
  if (!h) { printf("dlopen-fail %s\n", dlerror()); return 2; }
  int (*f)(void) = (int(*)(void))dlsym(h, "foo");
  printf("dlsym=%d\n", f ? f() : -1);
  return 0;
}
EOF
cat > "$W/t/exc.cc" <<'EOF'
#include <stdexcept>
#include <thread>
#include <cstdio>
int main(){
  int n = 0;
  std::thread th([&]{ n = 7; });
  th.join();
  try { throw std::runtime_error("x"); }
  catch (const std::exception &e) { printf("exc-ok %d %s\n", n, e.what()); }
  return 0;
}
EOF

echo "=== 1) 链接期 dev 名字与 ld.so 默认目录(这一组决定 -lc 是不是假的) ==="
for f in lib/toolchain/libc.so lib/toolchain/libc.so.6 lib/toolchain/libc_nonshared.a \
         lib/toolchain/libm.so lib/toolchain/libm.so.6 \
         lib/toolchain/libstdc++.so lib/toolchain/libstdc++.so.6 \
         lib/toolchain/libgcc_s.so lib/toolchain/libgcc_s.so.1 \
         lib64/ld-linux-x86-64.so.2 \
         usr/lib/x86_64-linux-gnu/libc.so.6 usr/lib/x86_64-linux-gnu/libc.so \
         lib/x86_64-linux-gnu/libc.so.6; do
  if inch "[ -e $f ] && echo Y" | grep -q Y; then ok "存在 $f"; else ng "存在 $f" "缺 $f"; fi
done

echo "=== 2) 裸命令动态编译 + 真运行(env -i, 不设任何库路径) ==="
# 用了 sqrt 就必须自己 -lm: 否则这条判据测的是"编译器有没有替我偷偷加库",
# 而不是动态链接本身(gcc 侧靠 libm.a 脚本恰好过、clang 侧红就是这个坑)。
has "gcc 编 C 动态 → 产物跑出预期字" "dyn-ok sqrt2=1.414214" 'cd /t && gcc -o a hello.c -lm && ./a'
has "gcc 产物有 PT_INTERP(确为动态可执行)" "ld-linux-x86-64.so.2" 'readelf -lW /t/a'
has "gcc 产物 DT_NEEDED 含 libc.so.6" "libc.so.6" 'readelf -d /t/a'
has "g++ 编 C++ 动态 → 产物跑出预期字" "cxx-dyn-ok" 'cd /t && g++ -o b hello.cc && ./b'
has "g++ 产物 DT_NEEDED 含 libstdc++.so.6" "libstdc++.so.6" 'readelf -d /t/b'
has "clang 编 C 动态 → 产物跑出预期字" "dyn-ok sqrt2=1.414214" 'cd /t && clang -o c hello.c -lm && ./c'
has "clang 产物 DT_NEEDED 含 libc.so.6" "libc.so.6" 'readelf -d /t/c'
has "clang++ 编 C++ 动态 → 产物跑出预期字" "cxx-dyn-ok" 'cd /t && clang++ -o d hello.cc && ./d'
has "clang++ 产物 DT_NEEDED 含 libstdc++.so.6" "libstdc++.so.6" 'readelf -d /t/d'

echo "=== 3) -fPIC -shared 出自家库, 再链接/dlopen(动态编译的第二半) ==="
has "gcc -fPIC -shared 出 libfoo.so.1(带 soname)" "libfoo.so.1" \
    'cd /t && gcc -fPIC -shared -Wl,-soname,libfoo.so.1 -o libfoo.so.1 libfoo.c && readelf -d libfoo.so.1'
# 装进 ld.so 的默认目录(宿主侧操作: chroot 里没有 cp/ln)
cp -f "$W/t/libfoo.so.1" "$W/usr/lib/x86_64-linux-gnu/" 2>/dev/null
ln -sf libfoo.so.1 "$W/usr/lib/x86_64-linux-gnu/libfoo.so" 2>/dev/null
cp -f "$W/t/libfoo.so.1" "$W/lib/x86_64-linux-gnu/" 2>/dev/null
for cc in gcc clang; do
  has "$cc 链接 -lfoo(不加 -L)→ 跑出 libfoo=42" "libfoo=42" "cd /t && $cc -o e-$cc uselib.c -lfoo && ./e-$cc"
done
has "产物 DT_NEEDED 记的是 libfoo.so.1(不是静态塞进来)" "libfoo.so.1" 'readelf -d /t/e-gcc'
rcis "链接一个不存在的库必须失败(不许静默退化)" 1 \
     'cd /t && gcc -o e-bad uselib.c -lnope'
has "dlopen 按 soname 在默认目录找到 libfoo.so.1" "dlsym=42" \
    'cd /t && gcc -o f usedl.c -ldl && ./f'

echo "=== 4) C++ 异常 + 线程(要 libstdc++.so.6 与 libgcc_s.so.1 同时在位) ==="
has "g++ -pthread 动态产物真跑出 exc-ok" "exc-ok 7 x" 'cd /t && g++ -pthread -o g exc.cc && ./g'
has "clang++ -pthread 动态产物真跑出 exc-ok" "exc-ok 7 x" 'cd /t && clang++ -pthread -o h exc.cc && ./h'

echo "=== 5) 静态那条路不许被这次改动弄坏(initramfs/装盘还靠它) ==="
has "gcc -static 产物真跑" "dyn-ok sqrt2=1.414214" 'cd /t && gcc -static -o s1 hello.c -lm && ./s1'
has "gcc -static 产物没有动态段(确为静态)" "There is no dynamic section" 'readelf -d /t/s1'
has "clang -static 产物真跑" "dyn-ok sqrt2=1.414214" 'cd /t && clang -static -o s2 hello.c -lm && ./s2'
has "g++ -static 的 C++ 产物真跑" "cxx-dyn-ok" 'cd /t && g++ -static -o s3 hello.cc && ./s3'

echo "=== 6) 不靠环境补丁与 rpath(交付里最容易退化的两条) ==="
has "env -i 下三个路径变量确实都为空" "LDP=[] CPATH=[] LIBRARY_PATH=[]" \
    'echo "LDP=[$LD_LIBRARY_PATH] CPATH=[$CPATH] LIBRARY_PATH=[$LIBRARY_PATH]"'
hasnt "动态产物里不许写 RUNPATH(运行时靠默认目录, 不靠编译时补丁)" "RUNPATH" 'readelf -d /t/a'
hasnt "动态产物里不许写 RPATH" "RPATH" 'readelf -d /t/a'
has "readelf 在 PATH 里可用(自查动态段的工具得在)" "There are" 'readelf -S /t/a'
has "-no-pie 那条路也真跑得起来(crt 混用会在这露馅)" "dyn-ok sqrt2=1.414214" \
    'cd /t && gcc -no-pie -o j hello.c -lm && ./j'

echo
echo "=== RESULT: pass=$P fail=$F ==="
exit $(( F > 0 ))
