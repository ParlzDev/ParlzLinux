#!/bin/sh
# run-iso.sh - 把 Parlz 安装到虚拟磁盘(交互式安装阶段)。
#
# 做的事: 用 QEMU 挂安装 ISO(El Torito 引导, 不借助 -kernel)+ 一块 virtio
# 虚拟磁盘, 开机后 body 自动跑 install: 写 MBR → 引导分区(syslinux,
# 镜像从 ISO 的 /boot/fat16.img 取)→ ext2 根分区 → cpfs 拷入整个 rootfs
# → 写 install-done 标记。看到 `=== install complete ===` 即装好。
#
# 装完按 Ctrl-A 再按 X 退出 QEMU, 然后跑 scripts/boot-disk.sh 从磁盘启动。
#
# 环境变量:
#   PARLZ_DISK      目标磁盘镜像路径(默认 /home/jgzyes/parlz-disk.img)
#   PARLZ_DISK_MB   磁盘大小 MiB(默认 512; 引导区占 64 MiB, 其余给根)
#   PARLZ_FRESH=1   先删掉旧磁盘镜像, 从零开始装(默认保留/新建)
#   PARLZ_AUDIO=0   不挂声卡(默认挂 ac97)
#   PARLZ_MEM       内存(默认 1024M)
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/run-iso.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
DISK=${PARLZ_DISK:-/home/jgzyes/parlz-disk.img}
DISK_MB=${PARLZ_DISK_MB:-512}
KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"

[ -f "$IMG/parlz-install.iso" ] || { echo "缺 $IMG/parlz-install.iso,先构建"; exit 1; }
[ -f "$IMG/parlz-bootfat.img" ] || echo "警告: 缺引导镜像, ISO 里可能也没有"

# QEMU -nographic 要的是**串口序**介质(/dev/console=ttyS0); 交付 ISO 是 vga 序
# (VMware/真机能敲键盘), 用它装盘的话安装进度和 shell 都在看不见的 VGA 上。
# 这里按需另导一份 e2e ISO 来跑, 交付那份不动。详见 scripts/serial-media.sh。
. /mnt/f/Linux/Parlz/scripts/serial-media.sh
if [ "${PARLZ_USE_DELIVERY_ISO:-0}" = "1" ]; then
    USE_ISO=$IMG/parlz-install.iso
    echo "PARLZ_USE_DELIVERY_ISO=1: 直接用交付 ISO(串口看不到安装进度/Shell)"
else
    parlz_ensure_serial_media || { echo "串口序介质准备失败"; exit 1; }
    USE_ISO=$PARLZ_SERIAL_ISO
fi

if [ "${PARLZ_FRESH:-0}" = "1" ]; then
    rm -f "$DISK"
fi
if [ ! -f "$DISK" ]; then
    echo "创建虚拟磁盘 $DISK (${DISK_MB} MiB)..."
    truncate -s "${DISK_MB}M" "$DISK"
fi

AUDIO_ARGS="-device ac97"
[ "${PARLZ_AUDIO:-1}" = "0" ] && AUDIO_ARGS=""

cat <<EOF
=== Parlz 安装到磁盘 ===
  ISO   : $USE_ISO
  目标盘: $DISK ($(du -h "$DISK" | cut -f1))
  声卡  : ${AUDIO_ARGS:-未挂}

装机步骤(自动, 约 1 分钟):
  1) 挂 ISO 到 /cdrom, 取 /boot/fat16.img 作引导镜像
  2) 写 MBR + 引导分区(syslinux VBR/ldlinux.sys/vmlinuz)
  3) 分区 2 建 ext2 并把整个 rootfs 拷进去
  看到 "=== install complete ===" 即装好。

装完按 Ctrl-A 再按 X 退出 QEMU, 然后执行:
  wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-disk.sh
EOF
echo

exec qemu-system-x86_64 \
  $KVM -m ${PARLZ_MEM:-1024M} -nographic -no-reboot \
  -serial mon:stdio \
  $AUDIO_ARGS \
  -cdrom "$USE_ISO" -boot d \
  -drive file="$DISK",if=virtio,format=raw,cache=none
