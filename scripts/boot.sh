#!/bin/sh
# boot.sh - 启动 Parlz (QEMU + KVM), 交互式 shell。
#
# 默认会把**引导镜像** images/parlz-bootfat.img 以只读盘挂进 guest
# (/dev/vdb), 并在 cmdline 传 parlz.bootimg=/dev/vdb —— 这样你在 guest 里
# 直接敲 `install` 就能把系统装到另一块盘。原因: install 不再内嵌引导镜像
# (那会造成自引用膨胀), 它需要外部来源; 没有来源会报 "找不到引导镜像"。
#
# 环境变量:
#   PARLZ_DISK     目标磁盘镜像(默认不挂; 设了就挂成 /dev/vda 供你 install)
#   PARLZ_DISK_MB  目标盘大小 MiB(默认 512, 仅 PARLZ_DISK 新建时用)
#   PARLZ_NO_IMG=1 不挂引导镜像盘(纯 shell 会话; install 将找不到引导镜像)
#   PARLZ_AUDIO=0  关闭声卡(默认开, 无需设置); 出声后端走宿主默认
#                   PULSE/ALSA; WSL2 无物理扬声器时音频落空, 但 guest 内
#                   /dev/snd/pcmC0D0p 链路完整(kernel snd-intel8x0 + devtmpfs)。
#   PARLZ_MEM      内存(默认 2048M, 工具链 initramfs 解压需 ~1.2G)
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"

AUDIO_ARGS="-device ac97"
[ "${PARLZ_AUDIO:-1}" = "0" ] && AUDIO_ARGS=""
[ -n "$AUDIO_ARGS" ] && echo "boot: 声卡已挂(ac97, 宿主默认音频后端)"

# 支持 PARLZ_MEM 环境变量覆盖内存(默认 2048M, 工具链 initramfs 解压需 ~1.2G)
MEM="${PARLZ_MEM:-2048M}"

# 引导镜像盘(只读) → guest 里 /dev/vdb; install 用它写引导分区
DRIVES=""
CMDLINE="console=ttyS0,115200"
if [ "${PARLZ_NO_IMG:-0}" != "1" ] && [ -f "$IMG/parlz-bootfat.img" ]; then
    DRIVES="$DRIVES -drive file=$IMG/parlz-bootfat.img,if=virtio,format=raw,readonly=on"
    CMDLINE="$CMDLINE parlz.bootimg=/dev/vdb"
    echo "boot: 引导镜像已挂为 /dev/vdb(只读) —— guest 里可直接 install"
fi
# 目标盘(可写) → guest 里 /dev/vda(在镜像盘之前挂, 保证是 vda 且被自动选中)
if [ -n "${PARLZ_DISK:-}" ]; then
    if [ ! -f "$PARLZ_DISK" ]; then
        echo "boot: 创建目标盘 $PARLZ_DISK (${PARLZ_DISK_MB:-512} MiB)"
        truncate -s "${PARLZ_DISK_MB:-512}M" "$PARLZ_DISK"
    fi
    DRIVES="-drive file=$PARLZ_DISK,if=virtio,format=raw,cache=none $DRIVES"
    echo "boot: 目标盘 $PARLZ_DISK 已挂为 /dev/vda(可写)"
fi

exec qemu-system-x86_64 \
  $KVM -m $MEM -nographic -no-reboot \
  -serial mon:stdio \
  $AUDIO_ARGS \
  -kernel "$IMG/parlz-bzImage" \
  -append "$CMDLINE" \
  -initrd "$IMG/parlz-initramfs" \
  $DRIVES
