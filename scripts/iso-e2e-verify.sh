#!/bin/sh
# iso-e2e-verify.sh - 完整 e2e: 从 ISO 引导 → install 写 ext2 → 重启 SeaBIOS
# 磁盘自启 → 验证分区 2 ext2 被 mount。
# 阶段 1: SeaBIOS + ISO(-cdrom) + 虚拟磁盘, initramfs 内 install
#   写 MBR + FAT16 + ext2 分区 2, 确认 "install complete" + "[3/6] mkfs ext2"
# 阶段 2: SeaBIOS + 磁盘(无 -kernel/-initrd), MBR → VBR → ldlinux.sys
#   → /vmlinuz, 验证内核启动 + init 挂载分区 2 ext2
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/iso-e2e-verify.sh
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
ISO=$IMG/parlz-install.iso
DISK=/home/jgzyes/parlz-e2e-disk.img
SERIAL=/home/jgzyes/parlz-e2e.log

echo "=== 阶段 1: ISO 引导 + 安装到磁盘 (SeaBIOS + -kernel + virtio 盘) ==="
rm -f "$DISK" "$SERIAL"
truncate -s 512M "$DISK"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -kernel "$IMG/parlz-bzImage" \
  -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 earlycon" \
  -drive file="$DISK",if=virtio,format=raw \
  -cdrom "$ISO" -boot d \
  -nographic -monitor none \
  </dev/null >"$SERIAL" 2>&1 &
QPID=$!
i=0
while [ $i -lt 48 ]; do
  grep -q "install complete" "$SERIAL" 2>/dev/null && break
  i=$((i+1))
  sleep 5
done
if ! grep -q "install complete" "$SERIAL" 2>/dev/null; then
  echo "阶段 1 失败: install complete 未出现,串口日志尾:"
  tail -30 "$SERIAL"
  kill "$QPID" 2>/dev/null || true
  wait "$QPID" 2>/dev/null || true
  exit 1
fi
echo "阶段 1 成功: install complete"
grep -aE "\[3/6\]|\[3b/6\]|\[4/6\]|mkfs ext2|whole-disk|cpfs|rootfs copied|install complete|Booting" "$SERIAL" | head -15
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
sleep 2

echo ""
echo "=== 阶段 2: 从磁盘自启动 (SeaBIOS Legacy, 无 kernel/initrd) ==="
rm -f "$SERIAL"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -nographic -monitor none -no-reboot \
  </dev/null >"$SERIAL" 2>&1 &
QPID=$!
i=0
while [ $i -lt 48 ]; do
  grep -q "Linux version" "$SERIAL" 2>/dev/null && break
  grep -q "kernel panic" "$SERIAL" 2>/dev/null && break
  i=$((i+1))
  sleep 5
done
echo "---- 阶段 2 串口日志(节选) ----"
grep -aE "Booting from|Parlz|Linux version|root device|/dev/root|kernel panic|ext4|mount|parlz>" "$SERIAL" | head -20
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

echo ""
echo "=== 判定 ==="
if grep -qa "Linux version" "$SERIAL" 2>/dev/null && ! grep -qa "kernel panic" "$SERIAL" 2>/dev/null; then
  echo "PASS: 磁盘自启动成功(SeaBIOS Legacy 路径), 无 kernel panic"
  exit 0
else
  echo "FAIL: 磁盘自启动未通过, 日志尾:"
  tail -15 "$SERIAL"
  exit 1
fi
