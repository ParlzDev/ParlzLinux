#!/bin/sh
# build.sh - 一键构建 Parlz (内核 + 用户空间 + initramfs + 镜像)。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/build.sh
#   或 Windows: sh build.bat
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

K=/home/jgzyes/parlz-kernel
[ -d "$K" ] || {
  echo ">>> 内核源码树未找到,先复制 Linux 源码到 WSL:"
  echo "    cp -a /mnt/f/Linux/Parlz/linux-7.2.5 $K"
  exit 1
}

echo "=== [1/3] 构建用户空间 + initramfs(CMake + GCC)==="
# userland 先构建(产出 rootfs 布局 + initramfs + 旧 vmlinuz 的 fat16 镜像)。
# gen-fatboot.sh 的 md5 校验保证 fat16_image 与 vmlinuz 一致。
sh /mnt/f/Linux/Parlz/scripts/build-userland.sh

echo "=== [2/3] 构建 Parlz 内核(嵌入 initramfs)==="
# 内核最后构建:build-kernel 同步 bzImage 到 root/boot/vmlinuz,
# 之后 diskboot 写入磁盘的 vmlinuz 与本次内核一致。
sh /mnt/f/Linux/Parlz/scripts/build-kernel.sh

echo "=== [3/3] 校验磁盘启动链一致性 ==="
# gen-fatboot 在 build-userland 里已生成 fat16_image;若 build-kernel 更新了
# vmlinuz 而未重跑 gen-fatboot,fat16_image 里的内核是旧版(磁盘启动卡死)。
# build-userland.sh 末尾的 md5 校验兜底:发现旧 vmlinuz 会报错提示重跑。
KZ=/home/jgzyes/parlz-kernel/arch/x86/boot/bzImage
[ -f "$KZ" ] || { echo "  无 bzImage,跳过校验"; exit 0; }
V1=$(md5sum /home/jgzyes/parlz-userland/root/boot/vmlinuz | cut -d' ' -f1)
V2=$(md5sum "$KZ" | cut -d' ' -f1)
if [ "$V1" != "$V2" ]; then
  echo "  root/boot/vmlinuz($V1) 与 bzImage($V2) 不一致"
  echo "  重跑 gen-fatboot.sh(用最新 vmlinuz 生成 fat16_image):"
  sh /mnt/f/Linux/Parlz/scripts/gen-fatboot.sh
  V1=$(md5sum /home/jgzyes/parlz-userland/root/boot/vmlinuz | cut -d' ' -f1)
  [ "$V1" = "$V2" ] || { echo "  仍不一致,检查 build-kernel.sh 同步步骤"; exit 1; }
fi
echo "  磁盘启动链:fat16_image <-> vmlinuz <-> bzImage 一致($V1)"

echo "=== 完成 ==="
echo "镜像: /mnt/f/Linux/Parlz/images/"
ls -l /mnt/f/Linux/Parlz/images/parlz-bzImage /mnt/f/Linux/Parlz/images/parlz-initramfs
