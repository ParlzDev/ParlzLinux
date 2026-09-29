#!/bin/sh
# build-busybox.sh - 在 WSL 中静态编译 busybox(用于 initramfs 的 /init)。
# 用法: wsl -d Ubuntu-26.04 -e bash -c 'sh /mnt/f/Linux/Parlz/scripts/build-busybox.sh'
#
# busybox 源码在 /home/jgzyes/busybox(git clone, 首次自动 clone)。
# 编译配置: 静态链接 + 关 PIE + 保留 init/ash/login/halt 核心 applet,
# 绕过 tc.c(WSL 内核 7.2.5 头文件移除 tc_cbq_* 结构体)。
# 产出: /home/jgzyes/busybox/busybox(静态)
#
# busybox-init 行为(与 init.c 的 mnt() 等价):
#   sysinit 行依次执行(EBUSY 无碍), 挂 proc/sys/devtmpfs/pts/tmp。
#   ttyS0 行用 askfirst+cttyhack, 每次串口输入都启动新 shell,
#   退出后回到 busybox login(或 ash -c "echo Parlz$ ")。
#   root 探测/root=pwd 检查留在 inittab sysinit 行(调用我们的
#   /bin/parlz-root-probe 脚本, 若不存在则跳过, 不影响基本启动)。
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

BB=/home/jgzyes/busybox
SRC=/mnt/f/Linux/Parlz/userland

# --- 1. clone(已存在则跳过) ---
if [ ! -d "$BB/.git" ]; then
  echo ">>> busybox 源码未找到,git clone..."
  git clone --depth 1 https://git.busybox.net/busybox "$BB"
fi
cd "$BB"

# --- 2. 生成 .config(defconfig + 强制静态/关 PIE/关 TC) ---
if [ ! -f .config ] || grep -q "CONFIG_STATIC is not set" .config 2>/dev/null; then
  echo ">>> [1/4] 生成 busybox defconfig"
  make defconfig >/dev/null 2>&1
  yes "" | make oldconfig >/dev/null 2>&1
fi

# 强制静态链接 + 关 PIE(CONFIG_STATIC=y, CONFIG_PIE 保持 not set)
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
grep -q "^CONFIG_STATIC=y" .config || echo "CONFIG_STATIC=y" >> .config
# 关 TC(WSL 内核 7.2.5 头文件缺 tc_cbq_* 结构体)
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
sed -i 's/^CONFIG_FEATURE_TC_INGRESS=y/# CONFIG_FEATURE_TC_INGRESS is not set/' .config
# 保留核心 applet: 确认 CONFIG_INIT/CONFIG_ASH/CONFIG_LOGIN 已开
for opt in CONFIG_INIT CONFIG_ASH CONFIG_LOGIN CONFIG_HALT CONFIG_MOUNT CONFIG_UMOUNT; do
  grep -q "^$opt=y" .config || echo "$opt=y" >> .config
done

# --- 3. 编译 ---
echo ">>> [2/4] 编译 busybox(静态)"
make oldconfig >/dev/null 2>&1 || true
make -j"$(nproc)" 2>&1 | tail -3
[ -f "$BB/busybox" ] || { echo "busybox 编译失败"; exit 1; }

# --- 4. 安装到 userland/busybox/(供 build-userland.sh 打包进 rootfs) ---
echo ">>> [3/4] 安装 busybox 到 userland/busybox/"
mkdir -p "$SRC/busybox"
cp -f "$BB/busybox" "$SRC/busybox/busybox"

# 生成 /etc/inittab(busybox-init 读它决定行为)。
# 关键: busybox parse_inittab 用 delims "#:" + config_read(4 token) 解析。
# 每行必须恰好 4 段(3 个冒号): tty:runlevel:action:command。
# command 段是整段,不能含冒号(冒号是 token 分隔符),否则 busybox 把
# ":xxx" 尾巴粘进命令名 → "can't run"。所以 cttyhack 不能写在
# command 后 —— busybox-init 已把控制台 tty 交给子进程,isatty 正常。
# sysinit 行只挂内核 init 未挂的 proc/sys/tmp;devtmpfs/devpts
# 由内核 CONFIG_DEVTMPFS(_MOUNT) 自动挂(见 build-kernel.sh defconfig),
# busybox-init 不再重复挂(避免 EBUSY)。挂载细节由 ::askfirst 的
# parlz-boot.sh 兜底(EBUSY 无碍)。
echo ">>> [4/4] 生成 /etc/inittab"
cat > "$SRC/etc-inittab" <<'INITEOF'
# busybox-inittab - Parlz initramfs /sbin/init(busybox-init)行为定义
#
# 关键: busybox parse_inittab 用 delims "#:" + config_read(4 token) 解析。
# 每行必须恰好 4 段(3 个冒号): tty:runlevel:action:command。
# command 段是整段,不能含冒号(冒号是 token 分隔符),否则 busybox 把
# ":xxx" 尾巴粘进命令名 → "can't run"。所以 cttyhack 不能写在
# command 后 —— busybox-init 已把控制台 tty 交给子进程,isatty 正常。
#
# 关键: busybox parse_inittab 用 delims "#:" + config_read(4 token) 解析。
# 每行必须恰好 4 段(3 个冒号): tty:runlevel:action:command。
# command 段是整段,不能含冒号(冒号是 token 分隔符),否则 busybox 把
# ":xxx" 尾巴粘进命令名 → "can't run"。所以 cttyhack 不能写在
# command 后 —— busybox-init 已把控制台 tty 交给子进程,isatty 正常。
#
# ::sysinit: 内核起 init 后立即执行的串行钩子。只挂 proc/sys/tmp,
# devtmpfs/devpts 内核 CONFIG_DEVTMPFS_MOUNT 已自动挂(EBUSY 无碍)。
#
# ::sysinit 跑启动主体(无需按键,开机自动):
#   /usr/local/bin/parlz-boot.sh(shim) → /bin/bash 跑
#   /usr/local/bin/parlz-boot-body.sh(root 探测/install.d/ifc/login/shell)。
# 之前用 ::askfirst 需人工按键才触发 body —— 非交互自动化(boot-verify
# 等)永远等不到, 且 body 的 login 钩子/shell 循环也进不去。改 sysinit
# 后开机即自动进入 body, 自动化脚本按固定节奏注入即可。
::sysinit:/bin/busybox mount -t proc proc /proc
::sysinit:/bin/busybox mount -t sysfs sysfs /sys
::sysinit:/bin/busybox mount -t tmpfs tmpfs /tmp
::sysinit:/usr/local/bin/parlz-boot.sh
::ctrlaltdel:/bin/busybox killall -HUP sh
::shutdown:/bin/busybox mount -o remount,ro /
INITEOF

echo "=== busybox 构建完成 ==="
ls -la "$SRC/busybox/busybox" "$SRC/etc-inittab"
file "$SRC/busybox/busybox" 2>/dev/null | cut -d: -f2-
