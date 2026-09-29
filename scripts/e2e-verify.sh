#!/bin/sh
# e2e-verify.sh - 端到端验证:QEMU 挂 ISO,init 自动跑安装器,重启从磁盘引导。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/e2e-verify.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
DISK=/tmp/parlz-e2e-disk.img
LOG=/tmp/parlz-e2e.log

rm -f "$DISK" "$LOG"
dd if=/dev/zero of="$DISK" bs=1M count=256 2>/dev/null

echo "=== Phase 1: 启动(挂 ISO + 虚拟磁盘),init 自动跑 /install.d ==="
rm -f "$LOG"
timeout 150 qemu-system-x86_64 \
  -m 512M -nographic -no-reboot \
  -serial file:"$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200" \
  -initrd "$IMG/parlz-initramfs" \
  -cdrom "$IMG/parlz-install.iso" \
  -drive file="$DISK",if=virtio,format=raw \
  >/dev/null 2>&1 || true

echo "--- install markers ---"
grep -aE "Parlz installer|\[[0-9]/5\]|install complete|MBR written|vmlinuz:|vda|virtio|auto-installer|no disk" "$LOG" | head -30
echo "--- disk MBR check ---"
python3 -c "
data = open('$DISK','rb').read()
print(f'MBR magic: {data[510:512].hex()} (expect 55aa)')
print(f'partition type @0x1BE: {data[0x1BE]:#04x} (expect 0x83)')
print(f'start LBA: {int.from_bytes(data[0x1C0:0x1C4],\"little\")}')
"

echo
echo "=== Phase 2: 重启从磁盘引导(不带 -cdrom) ==="
rm -f "$LOG.phase2"
timeout 90 qemu-system-x86_64 \
  -m 512M -nographic -no-reboot \
  -serial file:"$LOG.phase2" \
  -drive file="$DISK",if=virtio,format=raw \
  >/dev/null 2>&1 || true
echo "--- Phase 2: 磁盘引导成功? ---"
grep -ac "Parlz 0.1.0" "$LOG.phase2"
grep -aE "Parlz 0.1.0|Linux version|error|No filesystem|kernel panic" "$LOG.phase2" | head -10
echo "--- Phase 2 last 10 ---"
tail -10 "$LOG.phase2"
