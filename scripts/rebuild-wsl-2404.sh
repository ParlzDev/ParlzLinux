#!/bin/bash
# rebuild-wsl-2404.sh - 在 Ubuntu-24.04 重建 Parlz 构建环境
# Ubuntu-26.04 虚盘损坏后, 所有 F 盘数据完好, 只需重建 WSL 侧 /home/jgzyes
# 下的构建副本。跑完后 QEMU 可用 24.04(串口/stdbuf 无差异)。
# 用法: wsl -d Ubuntu-24.04 -u root -e bash /mnt/f/Linux/Parlz/scripts/rebuild-wsl-2404.sh
set -e
H=/home/jgzyes

echo "=== [1] 重建 parlz-kernel(7.2.5 源码, 从 F 盘拷) ==="
if [ ! -d $H/parlz-kernel/.git ]; then
  mkdir -p $H/parlz-kernel
  # 从 F 盘拷贝(排除 .git 加速; 保留全部源码+已改配置)
  cp -a /mnt/f/Linux/Parlz/linux-7.2.5/. $H/parlz-kernel/
  echo "  copied linux-7.2.5 -> $H/parlz-kernel ($(du -sh $H/parlz-kernel | cut -f1))"
fi
# 恢复关键内核改动: main.c /sbin/init 优先 + parlz 标识层
if [ -d /mnt/f/Linux/Parlz/linux-7.2.5 ]; then
  cp -f /mnt/f/Linux/Parlz/linux-7.2.5/init/main.c $H/parlz-kernel/init/main.c 2>/dev/null || true
  [ -f /mnt/f/Linux/Parlz/linux-7.2.5/include/linux/parlz.h ] && cp -f /mnt/f/Linux/Parlz/linux-7.2.5/include/linux/parlz.h $H/parlz-kernel/include/linux/parlz.h
  [ -f /mnt/f/Linux/Parlz/linux-7.2.5/arch/x86/kernel/parlz.c ] && cp -f /mnt/f/Linux/Parlz/linux-7.2.5/arch/x86/kernel/parlz.c $H/parlz-kernel/arch/x86/kernel/parlz.c
fi
# .config: 优先用 F 盘里的(含 parlz_defconfig 结果)
if [ -f /mnt/f/Linux/Parlz/.config-backup ]; then
  cp -f /mnt/f/Linux/Parlz/.config-backup $H/parlz-kernel/.config
fi
echo "=== [2] 重建 userland(dpkg/rpm/apt/yum/curl/wget/gcc/clang 二进制, 需 CMake) ==="
echo "  (跳过: userland 二进制已打进 rootfs.cpio.gz, 直接从 60M cpio 解包即可)"
echo "=== [3] 解包最新 rootfs.cpio.gz(已含 busybox/login/sh/bash 全部) ==="
mkdir -p $H/parlz-userland/root
cd $H/parlz-userland
zcat /home/jgzyes/parlz-kernel/rootfs.cpio.gz 2>/dev/null | cpio -idmu 2>/dev/null \
  || { echo "  尚无 60M rootfs.cpio.gz(旧 WSL 没拷出来), 从 images 拷"; \
       cp -f /mnt/f/Linux/Parlz/images/parlz-initramfs /home/jgzyes/parlz-kernel/rootfs.cpio.gz; \
       zcat /home/jgzyes/parlz-kernel/rootfs.cpio.gz | cpio -idmu; }
echo "  rootfs 解包: $(find . -type f | wc -l) 文件"
echo "=== [4] 建 src/(CMake 构建目录; 只需 login/sh 源码可重编) ==="
mkdir -p $H/parlz-userland/src
cp -a /mnt/f/Linux/Parlz/userland/. $H/parlz-userland/src/ 2>/dev/null || true
# CMake 需要
which cmake || (DEBIAN_FRONTEND=noninteractive apt-get install -y cmake 2>&1 | tail -1)
# ncurses(nano 重编时要用, 不强制)
echo "=== [5] 验证 QEMU + 工具链 ==="
qemu-system-x86_64 --version | head -1
gcc --version | head -1
echo "=== [6] 快速自测: 宿主编一个 hello.c 静态链接 ==="
echo 'int main(){return 42;}' > /tmp/t.c
gcc -static -o /tmp/t /tmp/t.c && /tmp/t; echo "  静态链接 rc=$?(42=正常, 宿主无 /dev/console)"
echo "=== REBUILD_2404_DONE ==="
