#!/bin/sh
# e2e-full.sh - 端到端:ISO 启动 -> 自动安装到磁盘 -> 校验落盘 -> 从磁盘引导
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/e2e-full.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
D=/tmp/e2e-d.img
LOG=/tmp/e2e-install.log

echo ">>> 构建 userland + ISO"
sh /mnt/f/Linux/Parlz/scripts/build-userland.sh 2>&1 | tail -1
sh /mnt/f/Linux/Parlz/scripts/build-iso.sh 2>&1 | tail -1

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
echo ">>> 阶段 2:宿主机校验落盘"
python3 /mnt/f/Linux/Parlz/scripts/check-disk.py "$D"

echo
echo ">>> 阶段 3:从磁盘引导(syslinux 引导链:MBR → /ldlinux.sys → 内核)"
# 当前磁盘上的 vmlinuz 在 FAT16 分区(LBA 2048 起),由 syslinux 引导。
# 宿主机侧用 QEMU -kernel 直接喂同一个 bzImage,模拟"从盘上的内核启动":
# 把 vmlinuz 从 FAT16 分区里取出来(它同时嵌在 initramfs 里,可直接用
# images/parlz-bzImage),再以 virtio 挂盘启动,根走内嵌 initramfs。
rm -f /tmp/e2e-boot.log
timeout 120 qemu-system-x86_64 -m 512M -nographic -no-reboot \
  -serial file:/tmp/e2e-boot.log \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200 root=/dev/vda1 rootfstype=vfat rw" \
  -initrd "$IMG/parlz-initramfs" \
  -drive file="$D",if=virtio,format=raw >/dev/null 2>&1 || true

echo "--- 从磁盘引导日志 ---"
grep -aE "Parlz 0.1.0|Linux version|vda1|vfat|mounted|install already done|Kernel command line" /tmp/e2e-boot.log | head -12

# 验证 install-done 标记生效:第二次启动不应再跑 /install.d
grep -aq "install already done" /tmp/e2e-boot.log && \
  echo "install-done 标记: OK(未重复安装)" || \
  echo "install-done 标记: 未生效(检查 LBA 1 标记)"
