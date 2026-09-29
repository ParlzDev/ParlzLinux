#!/bin/sh
# netbuild-check.sh - 检查内核网络子系统编译状态
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
K=/home/jgzyes/parlz-kernel
cd $K
make ARCH=x86_64 vmlinux >/tmp/vmlinux-build.log 2>&1
echo "vmlinux build rc=$?"
tail -3 /tmp/vmlinux-build.log
echo "=== vmlinux 网络符号 ==="
nm vmlinux 2>/dev/null | grep -cE "virtio_net|udp_|tcp_"
nm vmlinux 2>/dev/null | grep -E "T virtio_net|udp_v4_|tcp_v4_" | head -5
