#!/bin/sh
# build-userland.sh - 在 WSL 中用 CMake + GCC 构建 Parlz 用户空间并打包 initramfs。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/build-userland.sh
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

US=/home/jgzyes/parlz-userland
SRC=/mnt/f/Linux/Parlz/userland
IMG=/mnt/f/Linux/Parlz/images

echo ">>> [0/5] 生成 syslinux 引导组件 C 数组(mbr.bin + ldlinux.c32)"
# 把 /usr/lib/syslinux 的现成组件转成 userland/syslinux.h(静态链接用),
# 让 install 写 LBA 0 的 MBR 时用 syslinux mbr.bin。
sh /mnt/f/Linux/Parlz/scripts/gen-syslinux-header.sh || \
  echo "    WARNING: gen-syslinux-header 失败,install 将无 syslinux MBR"

echo ">>> [0c/5] 生成 syslinux MBR 头(userland/mbrbin.h)"
# install.c 写 LBA 0 用 syslinux 自带的 MBR 引导码(440 字节)。自写 MBR
# 踩过的坑: 直接把 VBR 读进 0x7C00 会覆盖**正在执行**的 MBR 本体, INT13
# 返回后继续执行的就是 VBR 的字节 → 跑飞(S ~"Booting from Hard Disk..."
# 后无任何输出)。syslinux mbr.bin 的做法是先把自身搬运到 0x0600 再读
# VBR 到 0x7C00, 再远跳 —— 成熟且已验证。
MBRSRC=/usr/lib/syslinux/mbr/mbr.bin
if [ -f "$MBRSRC" ]; then
  {
    echo '/* mbrbin.h - syslinux MBR 引导码(440 字节), 由 build-userland.sh 生成。'
    echo " * 源: $MBRSRC"
    echo ' * 作用: install.c 填 LBA 0 的前 440 字节。该码先把自身搬运到 0x0600,'
    echo ' * 再读活动分区的 VBR 到 0x7C00 并远跳过去(自搬运是必需的, 否则'
    echo ' * 读 VBR 会覆盖正在执行的 MBR 本体)。'
    echo ' * 分区表(@0x1BE)与 55AA 魔数由 install.c 运行时填。 */'
    echo '#ifndef MBRBIN_H'
    echo '#define MBRBIN_H'
    xxd -i -n syslinux_mbr_code "$MBRSRC"
    echo '#endif'
  } > "$SRC/mbrbin.h"
  echo "    mbrbin.h: $(wc -c < "$SRC/mbrbin.h") 字节"
else
  echo "    WARNING: 缺 $MBRSRC(apt-get install syslinux), install 无 MBR 引导码"
fi

echo ">>> [0b/5] 构建 busybox(静态,/sbin/init 用)"
sh /mnt/f/Linux/Parlz/scripts/build-busybox.sh || \
  echo "    WARNING: build-busybox 失败,initramfs 将无 busybox(用旧 init.c)"

echo ">>> [1/5] CMake 配置 + GCC 静态编译用户空间 + PazeSSL(TLS-SSH)"
rm -rf "$US"
mkdir -p "$US/build" "$US/root" "$US/src"
cp -a "$SRC"/. "$US/src/"
cd "$US"

# --- PazeSSL / PazeSSH(TLS-SSH)---
# 仅服务 pssh/pazessl 命令行工具;curl/wget 的 TLS 已切换到 OpenSSL(见下)。
# 源码固定在仓库根 TLS-SSH/。编 crypto+ssl+util+ssh 四个子目录
# (pssh 需要 ssh_* 符号),静态库 paze.a,pssh/pazessl 共用。
PAZE_DIR=/mnt/f/Linux/Parlz/TLS-SSH
[ -d "$PAZE_DIR" ] || { echo "    缺 $PAZE_DIR,跳过 PazeSSL(pssh 不可用)"; PAZE_DIR=""; }
PAZE_A="$US/paze.a"
if [ -n "$PAZE_DIR" ]; then
  mkdir -p "$US/paze-obj"
  rm -f "$US/paze-obj"/*.o "$PAZE_A"
  for f in $(find "$PAZE_DIR/src/crypto" "$PAZE_DIR/src/ssl" \
                "$PAZE_DIR/src/util" "$PAZE_DIR/src/ssh" -name "*.c" 2>/dev/null); do
    b=$(basename "$f" .c)
    gcc -O2 -fno-pie -fno-stack-protector -std=c17 -D_GNU_SOURCE \
        -I "$PAZE_DIR/include" -c "$f" -o "$US/paze-obj/$b.o" \
      2>"$US/paze-obj/$b.err" || { echo "    PazeSSL FAIL $b"; \
        head -3 "$US/paze-obj/$b.err"; }
  done
  ar rcs "$PAZE_A" "$US/paze-obj"/*.o
  echo "    PazeSSL lib: $(ls -la $PAZE_A | awk '{print $5}') bytes"
fi

# --- OpenSSL(静态库,scripts/build-openssl.sh 产出)---
# curl/wget 的 TLS 后端:完整 X509 链/主机名/IP 校验,--cacert 生效。
OSSL_PREFIX=/home/jgzyes/parlz-openssl/stage
OSSL_OK=""
[ -f "$OSSL_PREFIX/lib/libssl.a" ] && [ -f "$OSSL_PREFIX/lib/libcrypto.a" ] && OSSL_OK=1
[ -n "$OSSL_OK" ] || echo "    缺 $OSSL_PREFIX/lib/lib{ssl,crypto}.a,先跑 scripts/build-openssl.sh(curl/wget 退回明文 HTTP)"

# --- userland ---
# PM 自己的版本号(单一来源: 仓库根 .pm-release, 由 make-release.sh 生成)。
# 格式 组件+大.小-阶段+第几版, 阶段 R/RC/B/A; 没有就退回 dev 串。
PMVER="pm+0.0-A+0-dev"
PMREL=/mnt/f/Linux/Parlz/.pm-release
if [ -f "$PMREL" ]; then
    . "$PMREL"
    if [ -n "${PM_VERSION_ID:-}" ]; then PMVER="$PM_VERSION_ID"; fi
fi
echo "    PM 版本: $PMVER"
# 把两条库路径传给 CMake:OpenSSL 给 curl/wget,paze.a 给 pssh/pazessl
cmake -S src -B build -DCMAKE_BUILD_TYPE=Release \
      -DPAZE_A="$PAZE_A" -DPARLZ_OPENSSL_PREFIX="$OSSL_PREFIX" \
      -DPARLZ_PM_VERSION="$PMVER" >/dev/null
cmake --build build -j"$(nproc)"

# 生成 nano 需要的 revision.h(SOMETHING = 版本串,nano.c/winio.c 引用)
mkdir -p "$US/nano-gen"
echo '#define SOMETHING "nano 8.4 (Parlz)"' > "$US/nano-gen/revision.h"

# 工具链打包在 [2/5] rootfs 布局后执行(需要 $ROOT/usr/bin 已存在)
# 见下文 "--- 工具链打包" 段

# --- nano(文本编辑器,静态链接 ncursesw)---
# nano 源码在 userland/nano/(仓库内)。不走 autotools configure
# (它检测不到 WSL 的静态 ncursesw),直接用 gcc 编 src/*.c:
#   - revision.h 由上面生成
#   - 补 -DNANO_REG_EXTENDED / VERSION / PACKAGE_STRING 等 configure 宏
#   - 静态链接 -lncursesw -ltinfo,全自包含
NANO_DIR="$US/src/nano"
if [ -d "$NANO_DIR" ]; then
  echo "    编译 nano(静态 ncursesw)"
  gcc -O2 -fno-pie -fno-stack-protector -std=gnu11 -D_GNU_SOURCE \
      -I"$NANO_DIR/src" -I"$US/nano-gen" \
      -DNANO_REG_EXTENDED=REG_EXTENDED \
      -DVERSION='"8.4"' \
      -DPACKAGE_STRING='"GNU nano 8.4 (Parlz)"' \
      -DPACKAGE_VERSION='"8.4"' \
      -static "$NANO_DIR/src/"*.c \
      -o build/bin/nano -lncursesw -ltinfo -lm -lpthread \
      || echo "    WARNING: nano 编译失败,rootfs 将无 nano"
  echo "    nano: $(ls -la build/bin/nano 2>/dev/null | awk '{print $5}') bytes"
else
  echo "    缺 $NANO_DIR,跳过 nano"
fi

# 确认 curl/wget 真的链入了 OpenSSL(静态库符号在,HTTPS 可用)
for tool in curl wget; do
  if nm build/bin/$tool 2>/dev/null | grep -q 'SSL_connect'; then
    echo "    $tool HTTPS: OpenSSL 已链接 (SSL_connect 存在)"
  else
    echo "    WARNING: $tool 未链入 OpenSSL(HTTPS 不可用),检查 PARLZ_OPENSSL_PREFIX"
  fi
done

echo ">>> [2/5] 布局 rootfs"
ROOT="$US/root"
mkdir -p "$ROOT/bin" "$ROOT/sbin" "$ROOT/proc" "$ROOT/sys" "$ROOT/dev" \
  "$ROOT/etc" "$ROOT/parlz" "$ROOT/mnt" "$ROOT/boot" "$ROOT/tmp" "$ROOT/root" \
  "$ROOT/usr/local/bin"
# busybox-init: /sbin/init(busybox applet 多路复用, 自带 inittab 解析 +
# cttyhack + askfirst)。替代旧的 init.c(busybox-init 行为等价:
# proc/sys/devtmpfs 挂载 + ttyS0 交互 + install.d 钩子由 parlz-boot.sh 补)。
if [ -f "$SRC/busybox/busybox" ]; then
  cp "$SRC/busybox/busybox" "$ROOT/sbin/busybox"
  ln -sf busybox "$ROOT/sbin/init"      # /sbin/init → busybox
  echo "    /sbin/init = busybox-init(主)"
else
  echo "    WARNING: 无 busybox 二进制($SRC/busybox/busybox),/sbin/init 缺失!"
fi
# 兜底: 不删 /init。内核 wait_for_initramfs() 对 /init 做 init_eaccess,
# 成功则执行 /init(init.c, 完整挂载链+root 探测+安装器)。
# /sbin/init = busybox(4-token inittab, ::askfirst 触发 parlz-boot.sh)。
# 应急切 busybox-init: 启动参数加 rdinit=/sbin/init。
cp build/bin/init "$ROOT/init"
# inittab(busybox-init 行为定义)
[ -f "$SRC/etc-inittab" ] && cp "$SRC/etc-inittab" "$ROOT/etc/inittab"
# boot 脚本(busybox ::askfirst 触发: 环境/挂载/root 探测/install.d/ifc/shell 循环)
# 两段式: parlz-boot.sh(shim, askfirst 直接调) exec /bin/bash 跑
# parlz-boot-body.sh(真正逻辑, 全 POSIX, 不进 shx 判别器)。
[ -f "$SRC/parlz-boot.sh" ]    && cp "$SRC/parlz-boot.sh"    "$ROOT/usr/local/bin/parlz-boot.sh"
[ -f "$SRC/parlz-boot-body.sh" ] && cp "$SRC/parlz-boot-body.sh" "$ROOT/usr/local/bin/parlz-boot-body.sh"
chmod +x "$ROOT/usr/local/bin/parlz-boot.sh" "$ROOT/usr/local/bin/parlz-boot-body.sh" 2>/dev/null
cp build/bin/sh     "$ROOT/bin/parlz-sh"
# GNU Bash 提供完整语法;shx 是 /bin/sh 判别器:交互 tty 进 Parlz shell,
# 脚本/-c 走 bash(POSIX 模式)——维护脚本与用户交互两边都正确。
sh /mnt/f/Linux/Parlz/scripts/build-bash.sh "$US/build/bin"
cp build/bin/bash "$ROOT/bin/bash"
cp build/bin/shx    "$ROOT/bin/sh"
# 原 shell 的文件操作内建仍可作为独立命令使用。
for applet in ls touch clear; do
  ln -s parlz-sh "$ROOT/bin/$applet"
done
cat > "$ROOT/root/.bashrc" <<'EOF'
export PS1='\u@\h:\w\$ '
export PATH=/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin
cd "$HOME"
EOF
mkdir -p "$ROOT/usr/share/licenses/bash"
cp /mnt/f/Linux/Parlz/third_party/licenses/bash/COPYING "$ROOT/usr/share/licenses/bash/"
cat > "$ROOT/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/bin/bash
EOF
cat > "$ROOT/etc/group" <<'EOF'
root:x:0:
EOF
# 不再提供 busybox 兼容软链(Parlz 的 sh 是独立的最小 shell,
# 不是 busybox 多调用 applet;软链到 busybox 会让外部脚本误判为 busybox
# 并调用 applet 子命令如 busybox grep,那些不存在)。
# nano 是独立 gcc 直接编的(不在 CMake 目标里),单独拷
[ -f build/bin/nano ] && cp build/bin/nano "$ROOT/bin/nano"
for t in cat mount umount free dmesg mkdir rm file cp curl ifc ifconfig mkfs cpfs fdisk install boot mknod login user grep head tail wc sort sed awk which ps df ln mv tree ppm opkg ping wget frpc openvpn chmod audio w3m pweb pms pm
do
  cp "build/bin/$t" "$ROOT/bin/$t"
done
# PazeSSL / PazeSSH 命令行工具(OpenSSL/SSH 的 Paze 替代;有 paze.a 才拷)
# busybox applet 软链: 给 busybox 全 applet 建 /bin /usr/bin /sbin /usr/sbin
# 软链(与 userland 真命令冲突时保留真命令)。让 ash/busybox 习惯的
# /usr/bin/mount 等路径可命中(避免 inittab sysinit "can't run")。
sh /mnt/f/Linux/Parlz/scripts/gen-busybox-links.sh "$ROOT" "$ROOT/sbin/busybox"
chmod +x "$ROOT/sbin/busybox" "$ROOT/bin/"* 2>/dev/null
# busybox-init 钩子也要可执行
chmod +x "$ROOT/etc/inittab" "$ROOT/usr/local/bin/parlz-boot.sh" 2>/dev/null

# --- OPKG(真实上游 0.8.0 静态后端)+ PPM 委托层 ---
# build-opkg.sh 产出 /home/jgzyes/parlz-opkg/stage(二进制 + /etc/opkg
# 默认配置 + 上游 intercept 脚本 + 许可证),整树合并进 rootfs。
# 失败则中止:initramfs 里 ppm/opkg 必须指向真实 opkg-native。
sh /mnt/f/Linux/Parlz/scripts/build-opkg.sh || { echo "    缺 /home/jgzyes/parlz-opkg/stage,中止:opkg 无法合入 rootfs"; exit 1; }
[ -d /home/jgzyes/parlz-opkg/stage ] || { echo "    opkg stage 缺失(WSL 虚盘重建后需先跑 build-opkg.sh),中止"; exit 1; }
cp -a /home/jgzyes/parlz-opkg/stage/. "$ROOT/"

# initramfs 需要 /bin/sh 存在,且 init 的 execl 指向 /bin/sh

# --- 发布号: /etc/parlz-release 与内核 UTS_RELEASE 同源 ---
# .parlz-release 由 scripts/make-release.sh 生成; 日常增量构建没有它,
# 就退回内核头 include/linux/parlz-release.h 里的默认串 —— 两处必须一致,
# 否则 guest 里 cat /etc/parlz-release 和 /proc/version 会各说各话。
REL=/mnt/f/Linux/Parlz/.parlz-release
RH=/mnt/f/Linux/Parlz/linux-7.2.5/include/linux/parlz-release.h
gh() { sed -n "s/^#define $1[[:space:]]*\"\(.*\)\".*/\1/p" "$RH"; }
if [ -f "$REL" ]; then
    . "$REL"
else
    PARLZ_RELEASE_ID=$(gh PARLZ_RELEASE_ID)
    PARLZ_VERSION=$(gh PARLZ_VERSION)
    PARLZ_STAGE=$(gh PARLZ_STAGE)
    PARLZ_BUILDER=$(gh PARLZ_BUILDER)
    PARLZ_BUILD_TZ=$(gh PARLZ_BUILD_TZ)
    PARLZ_BUILD_TIME=$(gh PARLZ_BUILD_TIME)
fi
[ -n "$PARLZ_RELEASE_ID" ] || { echo "取不到发布号($REL / $RH 都没有 PARLZ_RELEASE_ID)"; exit 1; }
cat > "$ROOT/etc/parlz-release" <<EOF
name: ParlzOS
version: $PARLZ_RELEASE_ID
semver: $PARLZ_VERSION
stage: $PARLZ_STAGE
builder: $PARLZ_BUILDER
build-tz: $PARLZ_BUILD_TZ
build-time: $PARLZ_BUILD_TIME
kernel-release: 7.2.5-$PARLZ_RELEASE_ID
EOF
echo "Parlz $PARLZ_VERSION ($PARLZ_RELEASE_ID)" > "$ROOT/parlz/banner"
echo "    发布号: $PARLZ_RELEASE_ID"

# terminfo 数据库:静态 ncurses(nano 依赖)运行时按
# TERMINFO_DIRS=/etc/terminfo:/lib/terminfo:/usr/share/terminfo 查条目。
# initramfs 里没有就会报 "cannot initialize terminal type"。
# 只拷几个常用术语条目(每个 1~4KB),放 /usr/share/terminfo。
# /etc/terminfo 建软链指过去(放 /etc 下,init 的 /etc 目录已建好)。
TDB=/usr/share/terminfo
mkdir -p "$ROOT/usr/share"
for t in l/linux v/vt100 v/vt102 v/vt220 v/vt52 \
         x/xterm x/xterm-256color x/xterm-color x/xterm-mono x/xterm-vt220; do
  [ -f "$TDB/$t" ] || continue
  td="$ROOT/usr/share/terminfo/${t%/*}"
  mkdir -p "$td"
  cp "$TDB/$t" "$td/${t#*/}"
done
ln -sf /usr/share/terminfo "$ROOT/etc/terminfo"

# --- 防 initramfs 膨胀: root/boot/vmlinuz 恒为 8KB 占位。
# 内核启动不需要它(initramfs 本身就是根),而真 vmlinuz 会随 initramfs
# 一起被烘进内核 → 下次打包又拷回 → 自引用膨胀(60M→120M→…写挂)。
# 真 vmlinuz 只进引导镜像 images/parlz-bootfat.img(见 build-kernel 末尾)。
rm -f "$ROOT/boot/vmlinuz"
head -c 8192 /dev/zero > "$ROOT/boot/vmlinuz"
chmod 755 "$ROOT/boot/vmlinuz"

# 注: 引导镜像(parlz-bootfat.img)的生成已移到 build-kernel 末尾 ——
# 镜像里必须放**当轮**的内核,而 build-userland 跑在 build-kernel 之前
# (内核要嵌本轮 rootfs)。旧版在 gen-fatboot 后重编 install 让它内嵌
# 镜像,那正是自引用膨胀的来源: install ≈ 镜像 ≈ vmlinuz ≈ install。
# 现在 install 运行时读镜像(见 install.c open_boot_image),顺序解耦。

# DNS 解析用 resolv.conf(curl/ping 走 glibc 解析器, 按这里列的顺序问)。
# 顺序与超时都是踩过坑的:
#  - 10.0.2.3 = QEMU user(-netdev user)内置 DNS 代理, 开发验收第一顺位;
#  - 8.8.8.8 / 1.1.1.1 = VMware/真机上的兜底(QEMU NAT 会把它 SNAT 出去);
#  - options timeout:1 attempts:1 —— glibc 默认每服 5s×2 次才换下一个,
#    第一个服务器不可达时 curl 会"看着像卡死"; 1s×1 次则一秒就往下走。
# guest 里 ifc dhcp 会用 DHCP 真发的 DNS **覆盖**这份默认值。
mkdir -p "$ROOT/etc"
cat > "$ROOT/etc/resolv.conf" <<'EOF'
nameserver 10.0.2.3
nameserver 8.8.8.8
nameserver 1.1.1.1
options timeout:1 attempts:1
EOF

# pm feed: 默认指向官方镜像站(www.parlz.com/feed —— 站点 web/feed 就是
# output/feed 的同一份内容)。早先这里焊的是宿主开发机 10.0.2.2:8765,
# 那份地址只在"QEMU + 我这台 WSL 起着 pm-server"时有效, 交付介质上是死链。
# 本地源怎么指: 环境变量 PM_FEED, 或 guest 里
#   /bin/busybox echo http://10.0.2.2:8765 > /etc/pm/feeds.conf
mkdir -p "$ROOT/etc/pm"
cat > "$ROOT/etc/pm/feeds.conf" <<'EOF'
http://www.parlz.com/feed
EOF
echo "    pm feed: http://www.parlz.com/feed(默认; 本地源用 PM_FEED/feeds.conf 覆盖)"

# OpenSSL 编译期 --openssldir=/etc/ssl:严格 HTTPS 校验从这里找信任库。
# 拷宿主 ca-certificates 全量束为 /etc/ssl/cert.pem(真实公网 CA 可验)。
if [ -f /etc/ssl/certs/ca-certificates.crt ]; then
  mkdir -p "$ROOT/etc/ssl/certs"
  cp /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/ssl/cert.pem"
  echo "    CA 束: $(grep -c 'BEGIN CERTIFICATE' /etc/ssl/certs/ca-certificates.crt) 个根证书 -> /etc/ssl/cert.pem"
else
  echo "    WARNING: 宿主无 ca-certificates,严格 HTTPS 公网校验将失败关闭"
fi

# 安装模式脚本:ISO 里放 /install.d,init 检测到就自动跑安装器。
# install 无参数时会自动从 /sys/block 探测第一个磁盘。
# 注意:我们的 sh 不支持 if/for 等控制结构,install.d 只用纯命令序列。
cat > "$ROOT/install.d" <<'EOF'
#!/bin/sh
echo "=== Parlz auto-installer ==="
echo "installing to auto-detected disk..."
/bin/install
echo "=== install done ==="
EOF
chmod +x "$ROOT/install.d"

# 网络自测脚本(init 存在时自动跑,输出到串口)
# 2026-09-17: 扩展为本轮验收项(bash 特性/chmod/opkg 委托/curl HTTP+HTTPS)。
# 由 /bin/sh 执行 → shx 分流到 bash(POSIX 模式),||/&&/行续合法。
cat > "$ROOT/nettest.sh" <<'EOF'
#!/bin/sh
echo "=== TEST: bash 特性 ==="
bash -c 'arr=(a "b c" d); echo ARR=${arr[1]}'
bash -c 'echo BRACE={1..5}'
bash -c 'echo -e "E1\nE2"'
bash -c '[[ abc == a* ]]; echo GLOBQ=$?'
bash -c 'p(){ /bin/cat <(echo PROC_OK); }; p'
echo "=== TEST: chmod ==="
echo perm_test > /tmp/perm.txt
chmod -v 000 /tmp/perm.txt
cat /tmp/perm.txt && echo ROOT_READ_OK
chmod 644 /tmp/perm.txt
cat /tmp/perm.txt && echo CHMOD_RESTORE_OK
echo "=== TEST: grep 文件参数(修复验证) ==="
printf 'alpha one\nbeta two\nalpha three\n' > /tmp/gt.txt
grep alpha /tmp/gt.txt > /tmp/grep_out.txt
N=$(wc -l < /tmp/grep_out.txt)
[ "$N" -eq 2 ] && echo GREP_FILE_OK || echo "GREP_FILE_FAIL N=$N"
echo "=== TEST: opkg/ppm 委托 ==="
opkg --version
ppm opkg --version
echo "=== TEST: curl HTTP(host 10.0.2.2:8080) ==="
curl -s -m 10 -o /tmp/h.txt http://10.0.2.2:8080/hello.txt \
  && cat /tmp/h.txt && echo HTTP_OK
echo "=== TEST: curl 严格 HTTPS 无 CA 文件、系统束验证宿主 CA(应通过) ==="
curl -s -m 10 -o /tmp/ss.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/ss.txt && echo STRICT_SYS_OK
echo "=== TEST: curl 严格 HTTPS + --cacert(应通过) ==="
curl -s -m 10 --cacert /probe-ca.pem -o /tmp/hs.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/hs.txt && echo STRICT_CACERT_OK
echo "=== TEST: curl 严格 HTTPS + 假 CA(应失败关闭) ==="
printf 'not a certificate\n' > /tmp/badca.pem
curl -s -m 10 --cacert /tmp/badca.pem -o /dev/null https://10.0.2.2:8443/hello.txt \
  && echo STRICT_BADCA_UNEXPECTED \
  || echo STRICT_BADCA_REJECT
echo "=== TEST: curl --insecure TLS(对照) ==="
curl -s -m 10 --insecure -o /tmp/i.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/i.txt && echo INSECURE_OK
echo "=== TEST: wget HTTPS --ca-certificate=等号形式 ==="
wget -q --ca-certificate=/probe-ca.pem -O /tmp/ws.txt https://10.0.2.2:8443/hello.txt \
  && cat /tmp/ws.txt && echo WGET_CACERT_OK
echo "=== TEST: wget HTTP ==="
wget -q -O /tmp/w.txt http://10.0.2.2:8080/hello.txt \
  && cat /tmp/w.txt && echo WGET_OK
echo "=== 全部测试完成 ==="
echo E2E_FINISHED
EOF
chmod +x "$ROOT/nettest.sh"

# --- 工具链打包: 宿主的 GCC + Clang/LLVM(预编译 Linux 版)---
# 在 [2/5] 布局后执行: $ROOT/{opt,lib,etc,usr} 已存在。
# 工具链放 $ROOT/opt/toolchain/(gcc-<版本>/, llvm-<版本>/), glibc 在 /lib/toolchain/。
# 目录名由 build-toolchain.sh 按宿主实有版本决定(24.04 是 gcc-13/llvm-18,
# 旧实例是 gcc-15/llvm-21) —— 这里探测而不是写死, 否则换宿主后就算
# PARLZ_TC_BUNDLED=1 也静默不打进去。
TC=/home/jgzyes/toolchain-pack
TCG_NAME=$(ls -1 "$TC/opt" 2>/dev/null | sed -n 's|^gcc-||p' | sort -V | tail -1)
TCL_NAME=$(ls -1 "$TC/opt" 2>/dev/null | sed -n 's|^llvm-||p' | sort -V | tail -1)
if [ -d "$TC/opt/llvm-$TCL_NAME" ] && [ "${PARLZ_TC_BUNDLED:-0}" = "1" ]; then
  echo "    打包工具链(gcc-$TCG_NAME + llvm-$TCL_NAME)"
  mkdir -p "$ROOT/opt/toolchain" "$ROOT/lib/toolchain" "$ROOT/lib64" "$ROOT/etc/ld.so.conf.d" "$ROOT/usr/bin"
  cp -a "$TC/opt/gcc-$TCG_NAME" "$ROOT/opt/toolchain/gcc-$TCG_NAME"
  cp -a "$TC/opt/llvm-$TCL_NAME" "$ROOT/opt/toolchain/llvm-$TCL_NAME"
  cp -a "$TC/lib/." "$ROOT/lib/toolchain/"
  [ -f "$TC/lib64/ld-linux-x86-64.so.2" ] && cp -a "$TC/lib64/ld-linux-x86-64.so.2" "$ROOT/lib64/"
  [ -f "$TC/lib/ld-linux-x86-64.so.2" ] && [ ! -f "$ROOT/lib64/ld-linux-x86-64.so.2" ] && \
    cp -a "$TC/lib/ld-linux-x86-64.so.2" "$ROOT/lib64/"
  cat > "$ROOT/etc/ld.so.conf.d/toolchain.conf" <<TCEOF
/opt/toolchain/gcc-$TCG_NAME/lib
/opt/toolchain/llvm-$TCL_NAME/lib
/lib/toolchain
TCEOF
  ln -sf /opt/toolchain/gcc-$TCG_NAME/bin/gcc       "$ROOT/usr/bin/gcc"
  ln -sf /opt/toolchain/gcc-$TCG_NAME/bin/g++      "$ROOT/usr/bin/g++"
  ln -sf /opt/toolchain/gcc-$TCG_NAME/bin/cc       "$ROOT/usr/bin/cc"
  ln -sf /opt/toolchain/gcc-$TCG_NAME/bin/c++     "$ROOT/usr/bin/c++"
  ln -sf /opt/toolchain/llvm-$TCL_NAME/bin/clang  "$ROOT/usr/bin/clang"
  ln -sf /opt/toolchain/llvm-$TCL_NAME/bin/clang++ "$ROOT/usr/bin/clang++"
  ln -sf /opt/toolchain/llvm-$TCL_NAME/bin/llvm-config "$ROOT/usr/bin/llvm-config"
  # /bin 便捷名: 单跳绝对软链指包内目标(旧脚本/文档写 /bin/gcc 也能命中)。
  # 注意: 不用 ../usr/bin/gcc 双跳相对链 —— cpio 解包时内核按打包顺序写软链,
  # /bin/gcc 先于 /usr/bin/gcc 解出时, 双跳第二跳目标还没解出 → path lookup
  # ENOENT → guest 里 "./gcc: not found or not executable"。单跳指 /opt/toolchain
  # (包先拷, 解包顺序上一定早于 /bin/gcc), 一次命中, 彻底绕开解包竞态。
  for l in gcc g++ cc c++ clang clang++ llvm-config; do
    case $l in
      clang*) t=/opt/toolchain/llvm-$TCL_NAME/bin/clang;;
      c++|g++) t=/opt/toolchain/gcc-$TCG_NAME/bin/g++;;
      cc) t=/opt/toolchain/gcc-$TCG_NAME/bin/cc;;
      llvm-config) t=/opt/toolchain/llvm-$TCL_NAME/bin/llvm-config;;
      *) t=/opt/toolchain/gcc-$TCG_NAME/bin/gcc;;
    esac
    ln -sf "$t" "$ROOT/bin/$l"
  done
  # 驱动硬编码的 /usr/lib/x86_64-linux-gnu/{libm-2.43.a,libc.so.6,...} 绝对路径:
  # 包内造 /usr/lib 软链树指回 /lib/toolchain, 让 guest 里驱动注入的绝对路径可命中。
  mkdir -p "$ROOT/usr/lib/x86_64-linux-gnu"
  for f in "$ROOT/lib/toolchain/"*; do
    b=$(basename "$f")
    ln -sf "/lib/toolchain/$b" "$ROOT/usr/lib/x86_64-linux-gnu/$b" 2>/dev/null
  done
  # gcc 驱动(收集器)硬编码的版本化静态库 libm-2.43.a/libmvec.a:
  # 系统里 libm-2.43.a 是 ld script(GROUP 指向 libm.a+libmvec.a), 包内无该脚本;
  # 直接拷真实 .a 档案(宿主 2.43 同源)进软链树
  cp -aL /usr/lib/x86_64-linux-gnu/libm-2.43.a "$ROOT/usr/lib/x86_64-linux-gnu/libm-2.43.a" 2>/dev/null || true
  cp -aL /usr/lib/x86_64-linux-gnu/libmvec.a "$ROOT/usr/lib/x86_64-linux-gnu/libmvec.a" 2>/dev/null || true
  # gcc 私目录: /usr/lib/gcc/x86_64-linux-gnu/15 → 包内 G15
  mkdir -p "$ROOT/usr/lib/gcc/x86_64-linux-gnu"
  ln -sf "/opt/toolchain/gcc-$TCG_NAME/lib/gcc/x86_64-linux-gnu/15" \
    "$ROOT/usr/lib/gcc/x86_64-linux-gnu/15"
  # 系统 C 头文件: C++ 标准库 #include_next <stdlib.h> 等需要系统 C 头,
  # gcc/clang 默认搜索 /usr/include(宿主 WSL 系统路径), guest 里建软链到
  # 包内 include(已含系统 C 头副本, build-toolchain.sh 打包时拷入)。
  if [ -d "$ROOT/opt/toolchain/gcc-$TCG_NAME/include" ]; then
    mkdir -p "$ROOT/usr"
    ln -sf "/opt/toolchain/gcc-$TCG_NAME/include" "$ROOT/usr/include" 2>/dev/null
  fi
  echo "    工具链: gcc-$TCG_NAME=$(du -sh $ROOT/opt/toolchain/gcc-$TCG_NAME | cut -f1), llvm-$TCL_NAME=$(du -sh $ROOT/opt/toolchain/llvm-$TCL_NAME | cut -f1)"
else
  echo "    缺 $TC, 跳过工具链打包(需先跑 scripts/build-toolchain.sh)"
fi

# --- [4.5/5] 按 pm-trim.list 把默认系统里不要的命令移出去 -----------------
# 名单里的名字有两种来源: busybox 软链(gen-busybox-links.sh 已经没建)与
# userland **真二进制**(下面物理移出)。移出去的文件留在 $US/trim-stage,
# build-pm-feed.sh 拿它打 core.pm; guest 里 pm install core 装回。
# 注意: 这里**不删 /sbin/busybox** —— 裁的只是"命令名", `busybox <命令>` 仍可用,
# 启动脚本按绝对路径调 /bin/busybox 的部分因此完全不受影响。
TLIST=/mnt/f/Linux/Parlz/userland/pm-trim.list
STAGE=$US/trim-stage
rm -rf "$STAGE"; mkdir -p "$STAGE"
if [ -f "$TLIST" ]; then
    TRIMMED_BIN=""
    for n in $(grep -v '^#' "$TLIST" | tr -d ' \t' | grep -v '^$'); do
        # KEEP: 名字撞车但那是 Parlz 自己的东西, 不能走 ——
        #   install = 我们的装盘安装器(与 busybox 的拷贝 applet 同名, 见
        #   gen-busybox-links.sh 的跨目录重名保护); 装盘链路依赖它。
        case " install " in *" $n "*) continue ;; esac
        for d in bin sbin usr/bin usr/sbin; do
            p="$ROOT/$d/$n"
            if [ -f "$p" ] && [ ! -L "$p" ]; then
                cp -a "$p" "$STAGE/$n" 2>/dev/null || { echo "    !! $p 移不出去"; continue; }
                rm -f "$p"
                TRIMMED_BIN="$TRIMMED_BIN $d/$n"
            fi
        done
    done
    for x in $TRIMMED_BIN; do echo "$x"; done | LC_ALL=C sort > "$ROOT/etc/pm/trimmed-binaries"
    echo ">>> [4.5/5] 默认命令裁剪: 真二进制移出 $(wc -l < "$ROOT/etc/pm/trimmed-binaries") 个($(du -sh $STAGE | cut -f1)), applet 软链不建 $(wc -l < "$ROOT/etc/pm/trimmed-links" 2>/dev/null || echo 0) 个"
    echo "    留下的关键命令: /bin/install(安装器) /bin/mount /bin/cpfs /bin/login /bin/parlz-sh /sbin/busybox"
else
    echo ">>> [4.5/5] 无 $TLIST, 不裁剪"
fi

echo ">>> [5/5] 打包 initramfs (newc cpio + gzip)"
cd "$ROOT"
find . | LC_ALL=C sort | cpio -o -H newc 2>/dev/null | gzip -9 > "$US/initramfs.cpio.gz"
ls -l "$US/initramfs.cpio.gz"

mkdir -p "$IMG"
cp "$US/initramfs.cpio.gz" "$IMG/parlz-initramfs"
echo "=== userland + initramfs done: $IMG/parlz-initramfs ==="
