#!/bin/sh
# boot-verify.sh - 非交互启动验证:运行 120s 后退出,检查日志里的 Parlz 标记。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/boot-verify.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
LOG="$IMG/parlz-verify.log"
rm -f "$LOG"

KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"

timeout -k 10 120 qemu-system-x86_64 \
  $KVM -m 2048M -nographic -no-reboot \
  -serial file:"$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200 earlycon" \
  -initrd "$IMG/parlz-initramfs" >/dev/null 2>&1 || true

echo "=== verify markers in $LOG ==="
grep -ac "Parlz" "$LOG" && echo "PASS: parlz marker present" || echo "FAIL: no parlz marker"
grep -aE "Parlz 0.1.0|Parlz user-space|Parlz shell|parlz>" "$LOG" | head
echo "=== last 10 lines ==="
tail -10 "$LOG"
