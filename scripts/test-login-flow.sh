#!/bin/sh
# test-login-flow.sh - 验证 busybox-init 登录流: 首次设用户名密码 → 提示符
# 带用户 → 重启再登录(root= 探测 + install marker 双场景)。
# 用法: sh test-login-flow.sh <disk> [append] [timeout_s] [qemu]
#   参数1 = 磁盘(第一次新盘, 第二次同盘带 marker 验证跳过安装)
#   参数2 = 追加 cmdline(默认 root=/dev/vda2)
#   参数3 = 超时(默认 150)
#   参数4 = qemu 路径(默认 /usr/bin/qemu-system-x86_64)
set -e
DISK=$1
APPEND="console=ttyS0,115200 root=/dev/vda2"
[ -n "$2" ] && APPEND=$2
TIMEOUT_S=${3:-150}
QEMU=${4:-/usr/bin/qemu-system-x86_64}
IMG=/mnt/f/Linux/Parlz/images
LOG=/tmp/login-test.log
rm -f $LOG

# 注入 stdin: 等 55s 到 askfirst → 按键 → 首次登录设用户名/密码 → 进 shell 取提示符
(
  sleep 55
  printf "\n"
  sleep 2
  printf "tester\n"      # Username
  sleep 1
  printf "secret123\n"   # Password
  sleep 1
  printf "secret123\n"   # 确认
  sleep 3
  printf "whoami\n"      # 验证提示符用户
  sleep 1
  printf "ps\n"
  sleep 1
  printf "exit\n"
) | $QEMU \
  -m 512M -nographic -no-reboot \
  -kernel $IMG/parlz-bzImage \
  -append "$APPEND" \
  -initrd $IMG/parlz-initramfs \
  -drive file=$DISK,if=virtio,format=raw \
  -serial stdio -monitor none 2>/dev/null > $LOG

echo "=== 登录流关键行 ==="
grep -aE "Parlz user-space|first|首次|Username|Password|Password again|tester|whoami|root@|tester@|install.d|root device|boot ready|/dev/vda|install complete|network up|login rejected|login success|Parlz login" $LOG | tail -30
echo "=== 串口尾部 ==="
tail -15 $LOG
