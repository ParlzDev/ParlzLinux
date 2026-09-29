#!/bin/sh
# e2e-phase1.sh - 只跑阶段1:ISO 启动 -> init 自动安装到虚拟磁盘 -> 落盘校验
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/e2e-phase1.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
D=/tmp/e2e-d.img
LOG=/tmp/e2e-install.log

echo ">>> 阶段 1:ISO 启动,init 自动跑 /install.d"
rm -f "$D" "$LOG"
dd if=/dev/zero of="$D" bs=1M count=256 2>/dev/null
timeout 170 qemu-system-x86_64 -m 512M -nographic -no-reboot \
  -serial file:"$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200 parlz.install" \
  -initrd "$IMG/parlz-initramfs" \
  -cdrom "$IMG/parlz-install.iso" \
  -drive file="$D",if=virtio,format=raw >/dev/null 2>&1 || true

echo "--- 安装流程 ---"
grep -aE "auto-detected|\[[0-9]/5\]|MBR written|rescan|BLKPG|mkfs:|mounted filesystem|mounted at|kernel source|vmlinuz:|initramfs:|install complete|Can.t find|Invalid|No such|failed|ext4 error" "$LOG" | head -25
echo
echo "--- 完整日志(最后 40 行) ---"
tail -40 "$LOG"
echo
echo ">>> 宿主机校验落盘"
python3 /mnt/f/Linux/Parlz/scripts/check-disk.py "$D"
