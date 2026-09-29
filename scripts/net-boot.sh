#!/bin/sh
# net-boot.sh - QEMU 带网卡 + 虚拟磁盘启动 Parlz(验证 ifc / curl / 安装)。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/net-boot.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
DISK=/tmp/parlz-net-disk.img
LOG=/tmp/parlz-net.log

[ -f "$IMG/parlz-bzImage" ] || { echo "缺 $IMG/parlz-bzImage,先 build"; exit 1; }
[ -f "$IMG/parlz-initramfs" ] || { echo "缺 $IMG/parlz-initramfs,先 build"; exit 1; }
rm -f "$DISK"
dd if=/dev/zero of="$DISK" bs=1M count=256 2>/dev/null

# -netdev user:virtio-net,自动把 DNS(10.0.2.3)暴露给 guest;
# guest 里 ifc auto 配 10.0.2.15/24,网关 10.0.2.2,DNS 8.8.8.8(resolv.conf)
KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"

exec timeout 240 qemu-system-x86_64 \
  $KVM -m 512M -nographic -no-reboot \
  -serial file:"$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200 root=/dev/vda2 rootfstype=ext4 rw" \
  -initrd "$IMG/parlz-initramfs" \
  -drive file="$DISK",if=virtio,format=raw \
  -netdev user,id=n0 \
  -device virtio-net-pci,netdev=n0
