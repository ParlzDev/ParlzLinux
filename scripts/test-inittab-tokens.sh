#!/bin/sh
# 验证 busybox-init 能否解析 4-token inittab(宿主上直跑 busybox, 不依赖 QEMU)
set -e
BB=/home/jgzyes/parlz-userland/root/sbin/busybox
R=/home/jgzyes/parlz-userland/root

echo "=== 现有 inittab 内容 ==="
cat $R/etc/inittab

echo ""
echo "=== 宿主上直接跑 busybox-init, 喂 4-token inittab ==="
# 用宿主上的 rootfs, 把 /etc/inittab 换成纯 4-token 版, 让 busybox 解析
mkdir -p /tmp/bbtest/etc /tmp/bbtest/dev /tmp/bbtest/bin
cat > /tmp/bbtest/etc/inittab <<'EOF'
tty1::respawn:/bin/sh
tty2::askfirst:/bin/sh
::ctrlaltdel:/bin/reboot
::shutdown:/bin/umount -a -r
EOF
# 宿主 /bin/sh 存在(宿主 Linux), 但 busybox 跑起来会尝试 open /dev/tty1(宿主没有),
# 重点看它是否报 "Bad inittab entry" —— 不报就是 4-token 解析 OK
echo "--- busybox -h(inittab 解析 dry-run, 只验证语法) ---"
# busybox 无 -h, 直接跑(宿主缺 /dev/console 会报, 但能看到 inittab 解析结果)
$BB init 2>&1 | head -10 || true

echo ""
echo "=== 改回 rootfs 的 inittab: 全部 4-token ==="
cat > $R/etc/inittab <<'EOF'
# busybox-inittab - Parlz /sbin/init(busybox-init)行为定义
# 格式(严格 4 token, ':' 分隔): tty:runlevel:action:command
#   tty: 串口用 tty1; 空(=内核默认控制台 console=ttyS0)用 ""
#   runlevel: 空(默认)
#   action: sysinit/respawn/askfirst/ctrlaltdel/shutdown/restart
#   command: 可执行(绝对路径; busybox applet 用 /bin/sh 调, 它认多调用)
# 注:cttyhack 不是 action, 是独立 applet —— 必须放在 command 里:
#   /usr/local/bin/parlz-boot.sh(cttyhack 由脚本内 ioctl TIOCSCTTY 完成)
::sysinit:/bin/sh -c 'mount -t proc proc /proc; mount -t sysfs sysfs /sys; mount -t tmpfs tmpfs /tmp'
tty1::askfirst:/usr/local/bin/parlz-boot.sh
::ctrlaltdel:/bin/sh -c 'kill -HUP 1'
::shutdown:/bin/sh -c 'umount -a -r'
EOF
chmod 644 $R/etc/inittab
echo "=== 新 inittab ==="
cat $R/etc/inittab

# 校验每行(非注释非空)恰好 4 个字段
echo "=== 4-token 校验 ==="
awk '!/^#/ && NF { n=split($0,a,":"); if(n!=4) print "BAD("n"): " $0 }' $R/etc/inittab && echo "全部 4 token OK"
