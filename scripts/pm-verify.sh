#!/bin/sh
# pm-verify.sh - pm 包管理器端到端验收: 宿主起 pm-server(8765), QEMU guest 里
#   1) pm install gcc  → 工具链落 /opt/toolchain, gcc 能编译跑 C
#   2) pm install clang → clang 能编译
#   3) pm list 显示已装; nano 可用(TERMINFO_DIRS 修复)
# 全过打印 PM_ALL_OK, 否则 PM_FAIL。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/pm-verify.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
REPO=/home/jgzyes/pm-repo
[ -f "$IMG/parlz-initramfs" ] || { echo "缺 $IMG/parlz-initramfs"; exit 1; }
[ -f "$IMG/parlz-bzImage" ] || { echo "缺 $IMG/parlz-bzImage"; exit 1; }
[ -d "$REPO" ] || { echo "缺 pm-repo, 先跑 build-pm-packages.sh"; exit 1; }

# 1) 起 pm-server(宿主 8765; 已有则复用)
if ! curl -s -m 3 http://127.0.0.1:8765/Packages >/dev/null 2>&1; then
  echo ">>> 起 pm-server @ 0.0.0.0:8765"
  ( cd "$REPO" && nohup python3 -m http.server 8765 --bind 0.0.0.0 \
      > /home/jgzyes/pm-server.log 2>&1 & )
  sleep 2
fi
echo "pm-server: $(curl -s -m 3 http://127.0.0.1:8765/Packages | grep -c .) 行索引"

# 2) 注入 pmtest.sh 到 initramfs(不重打 CMake)
WORK=/home/jgzyes/parlz-pm-work
rm -rf "$WORK"; mkdir -p "$WORK"
cd "$WORK"
gunzip -c "$IMG/parlz-initramfs" | cpio -idm 2>/dev/null
    cat > pmtest.sh <<'EOF'
#!/bin/bash
# guest 内 pm 验收: 装 gcc + clang, 编译跑 C, 查 pm list, 测 nano
PASS=0; FAIL=0
ok(){ [ "$1" = 0 ] && { PASS=$((PASS+1)); echo "  PASS: $2"; } || { FAIL=$((FAIL+1)); echo "  FAIL: $2"; }; }

export LD_LIBRARY_PATH=/lib/toolchain:/opt/toolchain/gcc-15/lib:/opt/toolchain/llvm-21/lib
export PATH=/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin

# 基线: 默认 initramfs 不带 gcc/clang, 装之前 should not 找到
if command -v gcc >/dev/null 2>&1; then
  echo "  WARN: 装 gcc 前就有 gcc(非默认布局): $(command -v gcc)"
fi

echo "=== pm 验收: 安装 gcc ==="
pm install gcc; ok $? "pm install gcc"
echo "  安装后布局: PATH=$PATH"
ls -la /opt/toolchain/gcc-15/bin/gcc /usr/bin/gcc /bin/gcc /lib64/ld-linux-x86-64.so.2 2>&1 | head -6
which gcc; echo "  which gcc rc=$?"

echo "=== gcc 编译+跑(裸命令, 不传 -I -B -L; 默认动态链接) ==="
# 宿主 WSL 中间层会吞 bash heredoc/printf 里的 \n —— 生成 0 字节源文件
# (tooltest 踩过的坑)。C 源码用 echo 转义 \n 保证内容非 0:
echo '#include <stdio.h>
int main(){ printf("PM_CC_OK\n"); return 0; }' > /tmp/pm_c.c
G15=/opt/toolchain/gcc-15
LX="$G15/libexec/gcc/x86_64-linux-gnu/15"
L15="$G15/lib/gcc/x86_64-linux-gnu/15"
# 裸 gcc ./a.c: 头走 CPATH/默认 /usr/include(包内已带), as/ld 走
# /usr/bin 包内软链, 链接器自动找 /lib/toolchain 的 glibc。无 -I -B -L。
# 动态编译默认(无 -static), 产物动态链接, 运行时靠 LD_LIBRARY_PATH=/lib/toolchain。
gcc -o /tmp/pm_c /tmp/pm_c.c 2>/tmp/pm_c.err
R1=$?
# 运行前诊断: 产物类型 + 动态依赖(包内无 ldd/objdump 时退化为 file 判 ELF)
echo "  产物: $(file /tmp/pm_c 2>/dev/null | head -1)"
echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
ls -la /lib64/ld-linux-x86-64.so.2 /lib/toolchain/libc.so.6 2>&1 | head -3
/tmp/pm_c >/tmp/pm_c.out 2>&1
R2=$?
if [ $R1 -eq 0 ] && [ $R2 -eq 0 ] && grep -q PM_CC_OK /tmp/pm_c.out; then
  ok 0 "gcc 裸命令动态编译运行(头+as/ld 全命中, 产物动态)"
else
  ok 1 "gcc 编译运行 (编译rc=$R1 运行rc=$R2; err=$(head -1 /tmp/pm_c.err); out=$(head -1 /tmp/pm_c.out))"
fi

echo "=== pm 验收: 安装 clang ==="
pm install clang; ok $? "pm install clang"
# 裸 clang ./a.c (动态编译, 无 -B -L -I): 头走 CPATH/包内 /usr/include,
# as/ld 走 /usr/bin 包内软链(gcc 包装过), glibc 走 /lib/toolchain(LD_LIBRARY_PATH)。
# clang 驱动默认 -B 宿主 binutils, 裸跑靠 gcc 包装的 /usr/bin 软链。
clang -o /tmp/pm_c_cl /tmp/pm_c.c 2>/tmp/pm_cl.err
CL_LINK=$?
/tmp/pm_c_cl >/tmp/pm_cl.out 2>&1
CL_RUN=$?
if [ "$CL_LINK" -eq 0 ] && [ "$CL_RUN" -eq 0 ] && grep -q PM_CC_OK /tmp/pm_cl.out; then
  ok 0 "clang 裸命令编译运行(动态)"
else
  ok 1 "clang 编译运行 (linkrc=$CL_LINK runrc=$CL_RUN; err=$(head -2 /tmp/pm_cl.err | tr '\n' ';') out=$(head -1 /tmp/pm_cl.out))"
fi

# g++ 裸命令 (C++ 动态): 头走 CPATH 的 /usr/include/c++/15 + 包内 glibc 头
echo "=== g++ 裸命令 (C++ 动态) ==="
echo '#include <iostream>
int main(){ std::cout<<"PM_CXX_OK" << std::endl; return 0; }' > /tmp/pm_cxx.cpp
g++ -o /tmp/pm_cxx /tmp/pm_cxx.cpp 2>/tmp/pm_cxx.err
XX_LINK=$?
echo "  g++ link rc=$XX_LINK; err: $(head -1 /tmp/pm_cxx.err)"
/tmp/pm_cxx >/tmp/pm_cxx.out 2>&1
XX_RUN=$?
echo "  g++ run rc=$XX_RUN; out: $(head -1 /tmp/pm_cxx.out)"
if [ "$XX_LINK" -eq 0 ] && [ "$XX_RUN" -eq 0 ] && grep -q PM_CXX_OK /tmp/pm_cxx.out; then
  ok 0 "g++ 裸命令编译运行(C++ 动态)"
else
  ok 1 "g++ 编译运行 (linkrc=$XX_LINK runrc=$XX_RUN; err=$(head -1 /tmp/pm_cxx.err))"
fi

# 验证产物是动态 ELF (不是静态) —— 用户要的就是"动态编译"
echo "=== 动态链接验证 (file 看产物类型) ==="
FT=$(file /tmp/pm_c 2>/dev/null | head -1)
echo "  $FT"
case "$FT" in
  *"statically linked"*)
    ok 1 "gcc 产物是静态(应为动态)" ;;
  *"dynamically linked"*)
    ok 0 "gcc 产物为动态 ELF (动态编译生效)" ;;
  *)
    ok 1 "gcc 产物类型未知: $FT" ;;
esac

echo "=== pm list ==="
pm list; ok $? "pm list 无错"

    echo "=== nano(terminfo 条目可用验证) ==="
    echo hi > /tmp/nm.txt
    # guest 无交互 tty: nano 在 ncurses init 之后、isatty 检查处
    # die("Standard input is not a terminal") 是正常路径 —— 能走到
    # isatty 就证明 ncurses terminal type 解析成功(terminfo 条目命中)。
    # 条目缺失时 ncurses 在更早处报 "cannot initialize terminal type"
    # 直接 exit, 到不了 isatty。故判据 = 输出含 "Standard input is not
    # a terminal"(到达 isatty) 且无 "cannot initialize"。
    # guest 的静态 ncurses 无内置 dumb 条目(对照基准不可用), 用 vt220
    # 自证: 它由 build-userland.sh 拷入 /usr/share/terminfo/v/vt220。
    for tv in vt220 vt100 xterm; do
      TERM=$tv TERMINFO_DIRS=/etc/terminfo:/usr/share/terminfo \
        nano +1 /tmp/nm.txt </dev/null >/dev/null 2>/tmp/nano_err_$tv
      NV=$(cat /tmp/nano_err_$tv)
      echo "  nano($tv) err: [$NV]"
      if grep -q "Standard input is not a terminal" /tmp/nano_err_$tv; then
        # 到达 isatty 检查 = ncurses 已成功解析 $tv 条目 -> 命中
        ok 0 "nano($tv) terminfo 条目命中(到达 isatty, 无 terminfo 报错)"
      elif grep -q "cannot initialize terminal type" /tmp/nano_err_$tv; then
        ok 1 "nano($tv) 仍报 terminfo 未初始化(条目缺失/不可读)"
      else
        ok 1 "nano($tv) 输出异常(未达 isatty 也无 terminfo 报错: [$NV])"
      fi
    done
# 卸载 pm 装的包: 恢复默认布局(验证 pm remove + 默认不带 gcc/clang)。
# 判据 = 包内真实二进制是否还在: /usr/bin/gcc /bin/gcc 是包里的软链,
# pm remove 已删软链本体与目标树, 软链悬空后 access(X_OK) 不命中。
# 用包内路径而非 which —— 宿主 which 语义对悬空软链的行为不作保证,
# 而包内二进制存在性才是"是否已卸载"的直接证据。
echo "=== pm remove gcc/clang ==="
pm remove gcc; ok $? "pm remove gcc"
pm remove clang; ok $? "pm remove clang"
LEFT=0
[ -e /opt/toolchain/gcc-15/bin/gcc ] && LEFT=1
[ -e /opt/toolchain/llvm-21/bin/clang ] && LEFT=1
[ $LEFT -eq 0 ] && ok 0 "卸载后 gcc/clang 包内二进制已移除" \
                || ok 1 "卸载后 gcc/clang 包内二进制仍在(异常)"

echo "=== 汇总: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ] && echo "PM_ALL_OK" || echo "PM_FAIL"
EOF
chmod +x pmtest.sh
echo "    pmtest.sh 注入: $(wc -c < pmtest.sh) bytes"
# 宿主 WSL 中间层会吞 bash heredoc 里的内容 —— 注入脚本里的
# printf '...\n...' > 文件 会生成 0 字节源文件(同 tooltest 曾踩的坑)。
# 所以 pmtest.sh 一律用 base64 自解码生成 .c/.err 探测文件, 不走 heredoc/printf。
# (pmtest.sh 本身由宿主 cat > pmtest.sh <<'EOF' 生成, 内容是静态 heredoc, 不经
#  被吞路径; 只需保证它内部生成的 C 源文件非 0 字节。)
find . | LC_ALL=C sort | cpio -o -H newc 2>/dev/null | gzip -9 > /home/jgzyes/parlz-pm.cpio.gz
cd /; rm -rf "$WORK"
echo ">>> 注入 pmtest.sh 的 initramfs: $(ls -lh /home/jgzyes/parlz-pm.cpio.gz | awk '{print $5}')"

# 3) QEMU 起 guest(4G), 等 PM_ALL_OK
KVM=""; [ -w /dev/kvm ] && KVM="-enable-kvm"
LOG=/home/jgzyes/pm-verify.log
rm -f "$LOG"
qemu-system-x86_64 $KVM -m 4096M -nographic -no-reboot \
  -serial "file:$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200" \
  -initrd /home/jgzyes/parlz-pm.cpio.gz &
QPID=$!

OK=""
for i in $(seq 1 360); do
  if grep -q "PM_ALL_OK\|PM_FAIL\|init: pmtest finished" "$LOG" 2>/dev/null; then
    sleep 4; OK=1; break
  fi
  kill -0 "$QPID" 2>/dev/null || { sleep 2; grep -q "PM_ALL_OK\|PM_FAIL" "$LOG" 2>/dev/null && { OK=1; break; }; }
  sleep 1
done
[ -n "$OK" ] && kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== guest pm 判定 ==="
grep -E "pm install|PASS:|FAIL:|PM_ALL_OK|PM_FAIL|nano" "$LOG" 2>/dev/null | grep -vE "kworker|crng|ipxe" | head -40
if grep -q "PM_ALL_OK" "$LOG" && ! grep -q "PM_FAIL" "$LOG"; then
  echo ">>> PASS: pm 安装 gcc/clang + 编译运行 + nano 全通"
  exit 0
fi
echo ">>> FAIL: 见上方 guest 日志"
exit 1
