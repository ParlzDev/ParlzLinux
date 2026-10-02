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
# guest 内 pm + 工具链验收: 装完 gcc/clang 后**裸命令**动态编译并真运行。
# 三条硬规矩(都是以前判据松掉的地方):
#   1) 开头 unset LD_LIBRARY_PATH/CPATH/LIBRARY_PATH —— 那几个变量是 boot
#      脚本给的兜底, 带着它们测出来的"能编译"证明不了包自包含。
#   2) 判据取自产物的 readelf(DT_NEEDED/PT_INTERP)与**运行输出**, 不只看
#      编译器退出码, 也不用 `file`(我们的 file 不打 "dynamically linked",
#      以前那个 case 分支只会走 `*)` 恒判 FAIL)。
#   3) 工具链目录一律探测 /opt/toolchain/gcc-* 与 llvm-*, 不焊版本
#      (焊过 gcc-15/llvm-21 = 旧 26.04 宿主; 换 24.04 后那几条 ls/判据
#       指向不存在的目录, 等于没断)。
PASS=0; FAIL=0
ok(){ [ "$1" = 0 ] && { PASS=$((PASS+1)); echo "  PASS: $2"; } || { FAIL=$((FAIL+1)); echo "  FAIL: $2"; }; }
# 这条验收吃的是**宿主本地仓库**(scripts/pm-server.sh 那套, 端口 8765)。
# 为什么要在这里显式给: 这条 QEMU 走的是 `-kernel` + 无 rdinit → PID1 是我们的
# /init(userland/init.c), 它只在 PM_FEED 未设时填官网, 并且**不解析 cmdline 的
# parlz.feed=**(那个开关属于 busybox-init + boot body 那条正常路径)。
# 换源这件事本来就是"环境变量优先", 所以测试自己 export 才是对的, 别指望 -append。
export PM_FEED=http://10.0.2.2:8765
unset LD_LIBRARY_PATH CPATH LIBRARY_PATH CFLAGS CXXFLAGS LDFLAGS
export PATH=/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin

probe_tc(){ TCG=""; TCL=""; for d in /opt/toolchain/gcc-*;  do [ -d "$d" ] && TCG="$d"; done
                        for d in /opt/toolchain/llvm-*; do [ -d "$d" ] && TCL="$d"; done; }
probe_tc
# $1=描述 $2=期望输出字 $3=编译命令 $4=产物路径 —— 编译与运行两步都要断
run_case(){ local d="$1" want="$2" cmd="$3" bin="$4"
  sh -c "$cmd" >/tmp/c_o 2>/tmp/c_e; local lr=$?
  # 诊断里只用 cat 不用 head/tail: 这两个命令在默认系统里被裁进 core.pm 了,
  # 到 `pm install core` 之前都不存在 —— 用它们取错误信息会得到**空串**,
  # FAIL 就退化成"rc=1, 什么也没说"(实测就是这样看不清为什么红)。
  if [ $lr -ne 0 ]; then
    ok 1 "$d (编译 rc=$lr; err=$(cat /tmp/c_e); out=$(cat /tmp/c_o))"; return
  fi
  "$bin" >/tmp/c_r 2>&1; local rr=$?
  if [ $rr -ne 0 ] || ! grep -qF -- "$want" /tmp/c_r; then
    ok 1 "$d (运行 rc=$rr out=$(cat /tmp/c_r) err=$(cat /tmp/c_e))"; return
  fi
  ok 0 "$d"
}
need_case(){ readelf -d "$3" 2>/dev/null | grep -qF -- "$2"; ok $? "$1"; }

# 基线: 默认 initramfs 不带 gcc/clang
command -v gcc >/dev/null 2>&1 && echo "  WARN: 装 gcc 前就有 gcc: $(command -v gcc)"

echo "=== pm 验收: 安装 gcc ==="
pm install gcc; ok $? "pm install gcc"
probe_tc
echo "  工具链落点: ${TCG:-未发现 gcc-*}"
ls -l /usr/bin/gcc /usr/bin/g++ /usr/bin/readelf /lib64/ld-linux-x86-64.so.2 2>&1

# C/C++ 源用"多行 echo"生成: 宿主 WSL 中间层会吞 heredoc/printf 里的 \n
# → 0 字节源文件(tooltest 踩过)。
echo '#include <stdio.h>
int main(){ printf("PM_CC_OK\n"); return 0; }' > /tmp/pm_c.c
echo '#include <iostream>
int main(){ std::cout << "PM_CXX_OK" << std::endl; return 0; }' > /tmp/pm_cxx.cpp
echo 'int foo(void); int foo(void){ return 42; }' > /tmp/pm_lib.c
echo '#include <stdio.h>
extern int foo(void);
int main(){ printf("PM_LIB_OK %d\n", foo()); return 0; }' > /tmp/pm_use.c
echo '#include <stdio.h>
#include <dlfcn.h>
int main(){ void *h=dlopen("libpmfoo.so.1", RTLD_NOW); if(!h){ printf("DL_FAIL %s\n", dlerror()); return 2; }
  int (*f)(void)=dlsym(h,"foo"); printf("PM_DLOPEN_OK %d\n", f?f():-1); return 0; }' > /tmp/pm_dl.c
SZ=0
for f in pm_c.c pm_cxx.cpp pm_lib.c pm_use.c pm_dl.c; do
  # 判"非 0 字节"用 bash 内建 [ -s ]: 默认系统里 **wc 被裁进 core.pm 了**,
  # `[ "$(wc -c < f)" -gt 20 ]` 在 guest 里得到空串(bash 报 command not found),
  # -gt 拿空串比较必然假失败(实测打出 "只有  字节")。判据不许依赖被裁的命令。
  [ -s /tmp/$f ] || { echo "  SRC_FAIL: /tmp/$f 不存在或为空"; SZ=1; }
done
ok $SZ "五个测试源都非 0 字节"

echo "=== 裸命令动态编译 + 运行(无 -I/-B/-L, 无 LD_LIBRARY_PATH) ==="
run_case "gcc 编 C 动态并运行"    "PM_CC_OK"  "gcc -o /tmp/a_gcc /tmp/pm_c.c"         /tmp/a_gcc
need_case "gcc 产物 DT_NEEDED 含 libc.so.6" "libc.so.6" /tmp/a_gcc
readelf -lW /tmp/a_gcc 2>/dev/null | grep -q "ld-linux-x86-64.so.2"
ok $? "gcc 产物 PT_INTERP 指向 /lib64/ld-linux(不是静态)"
run_case "g++ 编 C++ 动态并运行"  "PM_CXX_OK" "g++ -o /tmp/b_gpp /tmp/pm_cxx.cpp"     /tmp/b_gpp
need_case "g++ 产物 DT_NEEDED 含 libstdc++.so.6" "libstdc++.so.6" /tmp/b_gpp

echo "=== pm 验收: 安装 clang ==="
pm install clang; ok $? "pm install clang"
probe_tc
echo "  工具链落点: ${TCL:-未发现 llvm-*}"
run_case "clang 编 C 动态并运行"   "PM_CC_OK"  "clang -o /tmp/c_cl /tmp/pm_c.c"        /tmp/c_cl
need_case "clang 产物 DT_NEEDED 含 libc.so.6" "libc.so.6" /tmp/c_cl
run_case "clang++ 编 C++ 动态并运行" "PM_CXX_OK" "clang++ -o /tmp/d_clpp /tmp/pm_cxx.cpp" /tmp/d_clpp

echo "=== 自家共享库: -fPIC -shared → 装进默认目录 → 链接 + dlopen ==="
run_case "gcc -fPIC -shared 出库 + -l: 链接 + 运行" "PM_LIB_OK 42" \
  "gcc -fPIC -shared -Wl,-soname,libpmfoo.so.1 -o /usr/lib/x86_64-linux-gnu/libpmfoo.so.1 /tmp/pm_lib.c \
   && gcc -o /tmp/e_use /tmp/pm_use.c -L/usr/lib/x86_64-linux-gnu -l:libpmfoo.so.1" /tmp/e_use
need_case "链接产物 DT_NEEDED 记的是 soname" "libpmfoo.so.1" /tmp/e_use
run_case "dlopen 按 soname 在默认目录命中" "PM_DLOPEN_OK 42" \
  "gcc -o /tmp/f_dl /tmp/pm_dl.c -ldl" /tmp/f_dl

echo "=== 静态那条路不许退化(initramfs 里的命令全靠它) ==="
run_case "gcc -static 编 C 并运行"   "PM_CC_OK"  "gcc -static -o /tmp/s1 /tmp/pm_c.c"     /tmp/s1
readelf -d /tmp/s1 2>&1 | grep -qi "no dynamic section"
ok $? "gcc -static 产物确无动态段"
run_case "g++ -static 编 C++ 并运行" "PM_CXX_OK" "g++ -static -o /tmp/s2 /tmp/pm_cxx.cpp" /tmp/s2

echo "=== 全程没把路径变量填回去(证明靠的是包, 不是 env) ==="
[ -z "${LD_LIBRARY_PATH:-}" ] && [ -z "${CPATH:-}" ] && [ -z "${LIBRARY_PATH:-}" ]
ok $? "LD_LIBRARY_PATH/CPATH/LIBRARY_PATH 仍为空"

echo "=== pm list ==="
pm list; ok $? "pm list 无错"

    echo "=== nano(terminfo 条目可用验证) ==="
    # nano 不在默认系统里 —— 第三轮瘦身把它移进了 core.pm(见 pm-trim.list),
    # 所以这里必须先 `pm install core` 把它装回来; 判据本身不变(见下面注释)。
    # 顺带这条也就是"被裁的命令能装回来且真跑起来"的判据。
    pm install core; ok $? "pm install core(取回被裁的 nano)"
    command -v nano >/dev/null 2>&1; ok $? "装 core 后 nano 解析得到"
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
# 判据 = 探测到的 $TCG/$TCL 下的真二进制是否还在(以前焊死 gcc-15/llvm-21,
# 那两行 [ -e ] 恒为假 —— 装没装上都不报, 属空断言)。
echo "=== pm remove gcc/clang ==="
pm remove gcc; ok $? "pm remove gcc"
pm remove clang; ok $? "pm remove clang"
pm remove core; ok $? "pm remove core"
probe_tc
LEFT=0
[ -n "$TCG" ] && [ -e "$TCG/bin/gcc" ] && LEFT=1
[ -n "$TCL" ] && [ -e "$TCL/bin/clang" ] && LEFT=1
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
  -append "console=ttyS0,115200 login.skip=1" \
  -initrd /home/jgzyes/parlz-pm.cpio.gz &
QPID=$!

# guest 里那 12 条编译 + 1.5 GB 下载解压要 1500s 量级(init.c 的 pmtest 看门狗
# 就是它), 宿主侧再多半分钟收尾。以前 360 圈 = 6 分钟, 判据还没跑完就被 kill,
# 报出来的 FAIL 是"没给时间"而不是产品问题。
OK=""
for i in $(seq 1 1620); do
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
