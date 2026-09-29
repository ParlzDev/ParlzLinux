#!/bin/sh
# boot-install.sh - 进到 Parlz **系统内**, 在 shell 里自己把系统装到磁盘。
#
# 与 run-iso.sh 的区别: 不是拿 ISO 当安装介质自动装, 而是把
#   ① 目标盘(可写 virtio, 默认 /dev/vda)
#   ② 引导镜像(只读 virtio, /dev/vdb ← images/parlz-bootfat.img)
# 一起挂进来, 内核/initramfs 走 -kernel/-initrd, cmdline 带
# `install.skip=1`(不自动装, 留给用户手动) + `parlz.bootimg=/dev/vdb`
# (告诉 install 引导镜像在哪)。登录后就是个普通 shell, 想装再装:
#
#   install            # 自动探测第一块可写盘(只读的 /dev/vdb 会被排除)
#   install /dev/vda   # 或显式指定目标盘
#   reboot             # 装完重启 → 本次会话直接从装好的盘启动
#
# install 会依次: 写 MBR → 写引导分区(镜像整体 pwrite 到 LBA 2048)
# → 分区 2 建 ext2 → cpfs 拷入整个 rootfs(软链照拷, 挂载点只建空目录)
# → 写 install-done 标记。目标盘正挂着当根时会**拒绝**(不能边跑边重写脚下
# 的盘), 那种情况请从 ISO/全新会话装。
# 也可先 fdisk 自己划盘, 再用 install 写引导+根。
# 装完 reboot: body 会逐个分区挑能挂成 ext2/ext4 的那个当根 → pivot_root
# 进**装好的系统**(四条判据的自动化验收: scripts/verify-user-install.sh)。
#
# 环境变量:
#   PARLZ_DISK      目标磁盘镜像路径(默认 /home/jgzyes/parlz-disk.img)
#   PARLZ_DISK_MB   磁盘大小 MiB(默认 512; 引导区占 64 MiB, 其余给根)
#   PARLZ_FRESH=1   先删掉旧盘从零开始
#   PARLZ_AUTO=1    不手动装, 让 body 开机自动装(等价 run-iso.sh 的无 ISO 版)
#   PARLZ_LOGIN=1   要求登录(默认带 login.skip=1 直接进 shell, 方便装盘)
#   PARLZ_AUDIO=0   不挂声卡
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-install.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
DISK=${PARLZ_DISK:-/home/jgzyes/parlz-disk.img}
DISK_MB=${PARLZ_DISK_MB:-512}
KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"

[ -f "$IMG/parlz-bzImage" ] || { echo "缺 $IMG/parlz-bzImage,先构建"; exit 1; }
[ -f "$IMG/parlz-initramfs" ] || { echo "缺 $IMG/parlz-initramfs,先构建"; exit 1; }
[ -f "$IMG/parlz-bootfat.img" ] || { echo "缺 $IMG/parlz-bootfat.img(先跑 build-kernel.sh)"; exit 1; }

[ "${PARLZ_FRESH:-0}" = "1" ] && rm -f "$DISK"
if [ ! -f "$DISK" ]; then
    echo "创建目标盘 $DISK (${DISK_MB} MiB)..."
    truncate -s "${DISK_MB}M" "$DISK"
fi

AUDIO_ARGS="-device ac97"
[ "${PARLZ_AUDIO:-1}" = "0" ] && AUDIO_ARGS=""

# cmdline: 默认不自动装 + 不要求登录(方便直接敲 install); PARLZ_AUTO=1 反过来
CMDLINE="console=ttyS0,115200 parlz.bootimg=/dev/vdb"
if [ "${PARLZ_AUTO:-0}" = "1" ]; then
    CMDLINE="$CMDLINE"
else
    CMDLINE="$CMDLINE install.skip=1"
fi
[ "${PARLZ_LOGIN:-0}" = "1" ] || CMDLINE="$CMDLINE login.skip=1"

cat <<EOF
=== 在系统内安装到磁盘 ===
  目标盘  : $DISK ($(du -h "$DISK" | cut -f1))  ->  guest 里是 /dev/vda(可写)
  引导镜像: $IMG/parlz-bootfat.img            ->  guest 里是 /dev/vdb(只读)
  cmdline : $CMDLINE

进入 shell 后:
  install             # 自动探测可写盘(排除只读的 /dev/vdb)并安装
  install /dev/vda    # 或显式指定
  reboot              # 装完重启, 本次会话就直接从装好的盘启动
退出 QEMU: Ctrl-A 然后 X
EOF
echo

exec qemu-system-x86_64 \
  $KVM -m ${PARLZ_MEM:-1024M} -nographic \
  -serial mon:stdio \
  $AUDIO_ARGS \
  -kernel "$IMG/parlz-bzImage" \
  -initrd "$IMG/parlz-initramfs" \
  -append "$CMDLINE" \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -drive file="$IMG/parlz-bootfat.img",if=virtio,format=raw,readonly=on
