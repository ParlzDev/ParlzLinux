#!/bin/sh
# disk-e2e.sh - 磁盘安装 → 自启动 端到端(Ubuntu-24.04 环境)。
#
# 阶段 1(安装):-kernel/-initrd 起 initramfs + 空目标盘(/dev/vda, 512 MiB)
#   + 引导镜像挂成只读 virtio 盘(/dev/vdb = images/parlz-bootfat.img)。
#   initramfs 内 busybox body → /install.d → install:
#     MBR(自写 INT13 AH=0x42 + DAP) + 引导分区(镜像整体 pwrite 到 LBA 2048)
#     + ext2 分区 2(mkfs) + cpfs 拷 rootfs + install-done 标记(LBA 1)。
#   成功标记 = "install complete"。
#
# 阶段 2(自启动):同一块盘裸引导(SeaBIOS, 无 -kernel/-initrd):
#   BIOS 读 LBA 0 的 MBR → INT13 扩展读 LBA 2048 的 syslinux VBR → 0x7C00
#   → VBR 读 /ldlinux.sys → /syslinux.cfg → LINUX /vmlinuz
#     + APPEND rdinit=/sbin/init root=/dev/vda2 → busybox-init
#   → body 挂 /dev/vda2(ext2)→ pivot_root 换真根 → login → 交互 shell。
#   成功标记 = "pivot_root OK, now running on installed root" + "Parlz shell"
#   (已安装根里的 shell 真起来了), 且无 kernel panic。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/disk-e2e.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
DISK=/home/jgzyes/parlz-e2e-disk.img
SERIAL=/home/jgzyes/parlz-e2e-serial.log
# 一路 -nographic, 装到盘上的 syslinux.cfg 与 vmlinuz 都取自这份引导镜像 ——
# 必须是**串口序 + 本轮内核**(交付序装出来的盘首启提示只上屏幕; 旧内核会让
# "判据看起来在验新代码"却跑在旧 initramfs 上)。判据都在 serial-media.sh 里,
# 所以这里无条件调它, 由守卫决定要不要重建。
. /mnt/f/Linux/Parlz/scripts/serial-media.sh
parlz_ensure_serial_media || exit 1
BOOTIMG=$PARLZ_SERIAL_BF

for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs" "$BOOTIMG"; do
    [ -f "$f" ] || { echo "缺 $f"; exit 1; }
done

echo "=== 阶段 1: 安装到磁盘(空盘 + 只读引导镜像盘) ==="
rm -f "$DISK" "$SERIAL"
truncate -s 512M "$DISK"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -kernel "$IMG/parlz-bzImage" \
  -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 earlycon login.skip=1 parlz.bootimg=/dev/vdb" \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -drive file="$BOOTIMG",if=virtio,format=raw,readonly=on \
  -nographic -monitor none -no-reboot \
  </dev/null >"$SERIAL" 2>&1 &
QPID=$!
i=0
while [ $i -lt 90 ]; do
  grep -qa "install complete" "$SERIAL" 2>/dev/null && break
  grep -qa "kernel panic" "$SERIAL" 2>/dev/null && break
  i=$((i+1)); sleep 5
done
if grep -qa "install complete" "$SERIAL" 2>/dev/null; then
  echo "阶段 1 成功: install complete"
  grep -aE "boot image source|MBR|VBR|partition 1|partition 2|ext2|cpfs|rootfs copied|install complete" "$SERIAL" | head -14
else
  echo "阶段 1 失败, 串口日志尾:"
  tail -30 "$SERIAL"
  kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
  exit 1
fi
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
sleep 2

echo ""
echo "=== 阶段 2: 同一块盘裸引导(MBR → VBR → syslinux → vmlinuz) ==="
rm -f "$SERIAL"
FIFO=/home/jgzyes/parlz-e2e.fifo; rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"   # 先占住写端, 否则 qemu 的 <FIFO 与"等日志"互等死锁
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -boot c -nographic -monitor none -no-reboot \
  <"$FIFO" >"$SERIAL" 2>&1 &
QPID=$!
# 首启要设置用户名/密码(磁盘上的 syslinux.cfg 不带 login.skip)
sh /mnt/f/Linux/Parlz/scripts/guest-first-login.sh "$SERIAL" "$FIFO" tester parlz123 300 || true
i=0
while [ $i -lt 60 ]; do
  grep -qa "Parlz shell" "$SERIAL" 2>/dev/null && break
  grep -qa "kernel panic\|not a bootable\|Boot error\|No working init" "$SERIAL" 2>/dev/null && break
  i=$((i+1)); sleep 2
done
echo "---- 阶段 2 串口日志(节选) ----"
grep -aE "Booting|Parlz|Linux version|root device|vda|mounted|pivot|login|console|ext4|ext2|panic|not a bootable|Boot error|SYSLINUX" "$SERIAL" | head -30
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true

echo ""
echo "=== 判定 ==="
# 光"见到 Username:/boot ready"不够 —— 空根也会打这些。必须真换根到
# 已安装的根, 且已安装根里的 shell 起得来。
if grep -qa "pivot_root OK, now running on installed root" "$SERIAL" 2>/dev/null \
   && grep -qa "Parlz shell" "$SERIAL" 2>/dev/null \
   && ! grep -qa "kernel panic" "$SERIAL" 2>/dev/null; then
  echo "PASS: 磁盘自启动成功(MBR → VBR → syslinux → 内核 → 挂根 → 换根 → shell)"
  exit 0
else
  echo "FAIL: 阶段 2 未进入已安装根的 shell, 日志尾:"
  tail -25 "$SERIAL"
  exit 1
fi
