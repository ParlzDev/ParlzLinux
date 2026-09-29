#!/bin/sh
# boot-disk.sh - 从**已安装的磁盘**启动 Parlz(不挂 ISO, 不挂 -kernel)。
#
# 链路: SeaBIOS 读 LBA 0 的 MBR(syslinux mbr.bin 引导码)→ INT13 读活动
# 分区 VBR 到 0x7C00 → syslinux VBR 读 /ldlinux.sys → /syslinux.cfg →
# LINUX /vmlinuz + APPEND rdinit=/sbin/init root=/dev/vda2 rootdelay=2
# → busybox-init → body 挂 /dev/vda2(ext2)→ pivot_root 换真根 → 登录。
#
# 前置: 先跑 scripts/run-iso.sh 把系统装到同一块盘
#       (或直接用本脚本加 -cdrom 也能装, 但 run-iso.sh 更直观)。
#
# 环境变量:
#   PARLZ_DISK     磁盘镜像路径(默认 /home/jgzyes/parlz-disk.img)
#   PARLZ_AUDIO=0  不挂声卡(默认挂 ac97)
#   PARLZ_MEM      内存(默认 1024M)
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/boot-disk.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

DISK=${PARLZ_DISK:-/home/jgzyes/parlz-disk.img}
KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"

[ -f "$DISK" ] || {
    echo "缺磁盘镜像 $DISK —— 先跑 scripts/run-iso.sh 安装"; exit 1; }

# 快速自检: LBA 0 有 MBR 魔数, LBA 2048 有引导扇区(EB ?? 90 + 55AA)。
# 两条要分开报: 只写进 MBR、引导分区却还是全零的"半成品盘"(装到一半失败过)
# 在 SeaBIOS 那边只会转成一句 `Missing operating system.`, 然后接着去试
# 软驱/光驱/网络, 看着像"引导器坏了", 其实是这块盘根本没装完。
if ! dd if="$DISK" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | grep -q "55 aa"; then
    echo "警告: $DISK 的 LBA 0 没有 55AA 引导签名, 可能还没装过系统"
elif ! dd if="$DISK" bs=1 skip=$((2048*512+510)) count=2 2>/dev/null | od -An -tx1 | grep -q "55 aa"; then
    echo "错误: $DISK 有 MBR/分区表, 但 LBA 2048 的引导分区没有有效引导扇区"
    echo "      —— 这是一块装到一半失败的盘(引导分区没落盘)。"
    echo "      换一块装好的盘, 或重装: PARLZ_FRESH=1 sh scripts/boot-install.sh"
    echo "      (直接启动只会得到 SeaBIOS 里的 'Missing operating system.')"
    exit 1
fi

AUDIO_ARGS="-device ac97"
[ "${PARLZ_AUDIO:-1}" = "0" ] && AUDIO_ARGS=""

cat <<EOF
=== 从磁盘启动 Parlz ===
  磁盘: $DISK ($(du -h "$DISK" | cut -f1))
  引导: MBR → VBR → syslinux → /vmlinuz → 挂 /dev/vda2 → pivot_root
  退出: Ctrl-A 然后 X
EOF

# 盘上 syslinux.cfg 的 console= 顺序决定谁能输入(内核取最后一个)。
# 交付序是 vga(/dev/console=tty0): 在 -nographic 下只有内核 printk 到串口,
# 登录提示与 shell 落在看不见的 VGA 上 —— 不是坏了, 是这块盘的介质序不对。
# 直接 grep 整块盘: syslinux.cfg 在引导分区里挨着 vmlinuz 之后, 只读开头
# 几个扇区会读不到(dd count=8192 就漏了, 于是提示永远不出现)。
_CFG=$(grep -a -m1 '^APPEND console=' "$DISK" 2>/dev/null)
[ -n "$_CFG" ] || _CFG="(没在盘里读到 APPEND 行 —— 引导分区可能没装好)"
echo "  控制台: $_CFG"
printf '%s\n' "$_CFG" | grep -q 'console=ttyS0,115200 console=tty0' && {
    echo "      ↑ /dev/console=tty0(VMware/真机才能用键盘)。要在本窗口交互, 用"
    echo "        串口序介质重装这份盘: sh /mnt/f/Linux/Parlz/scripts/run-iso.sh"
    echo "        (它会按需导 images/parlz-install.e2e.iso, /dev/console=ttyS0)"
}
echo

exec qemu-system-x86_64 \
  $KVM -m ${PARLZ_MEM:-1024M} -nographic -no-reboot \
  -serial mon:stdio \
  $AUDIO_ARGS \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -boot c
