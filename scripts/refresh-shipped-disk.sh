#!/bin/sh
# refresh-shipped-disk.sh - 重做 images/parlz-installed-disk.img(交付快照)。
#
# 为什么不能直接拿工作盘 cp 过来(2026-09-28 踩过): 验收脚本装出来的盘是
# **串口序 + 已经首启过**(tester/parlz123 那个账号就是脚本喂进去的), 拿它当
# 交付快照, 在 VMware 里开机就是一片黑(输出全在串口)、还带着别人的账号。
# 交付快照的两条约定:
#   ① **交付序**(/dev/console=tty0): 装出来的盘 syslinux.cfg 取自带内嵌的
#      引导分区镜像, 所以安装源必须用交付序那份 images/parlz-bootfat.img;
#   ② **没首启过**: 只在 QEMU 里装盘一步 + sync, 不喂任何凭证, 让收到盘的人
#      自己设账户(首启会在屏幕上要求设置)。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/refresh-shipped-disk.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
P=/mnt/f/Linux/Parlz
IMG=$P/images
DISK=/home/jgzyes/parlz-disk.img          # 工作盘(会被重装, 就是验收用的那块)
OUT=$IMG/parlz-installed-disk.img
BOOTIMG=$IMG/parlz-bootfat.img            # 交付序(不是 .e2e 那份串口序)
LOG=/home/jgzyes/parlz-snap.log
W=/home/jgzyes/parlz-snap
for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs" "$BOOTIMG"; do
    [ -f "$f" ] || { echo "缺 $f"; exit 1; }
done
# 交付序自检: 拿错成 .e2e 那份就等于把"黑屏盘"发出去
grep -aq 'APPEND console=ttyS0,115200 console=tty0' "$BOOTIMG" || {
    echo "refresh-shipped-disk: $BOOTIMG 不是交付序(vga) —— 别用它做快照"; exit 1; }

rm -rf "$W"; mkdir -p "$W"
echo ">>> [1] 用**交付序**引导镜像装一次盘(空盘 + 只读镜像盘)"
rm -f "$DISK" "$LOG"
truncate -s 512M "$DISK"
FIFO=$W/in; mkfifo "$FIFO"
exec 3<>"$FIFO"
qemu-system-x86_64 -m 1024M -nographic -no-reboot \
  -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 install.skip=1 login.skip=1 parlz.bootimg=/dev/vdb" \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -drive file="$BOOTIMG",if=virtio,format=raw,readonly=on \
  <"$FIFO" >"$LOG" 2>&1 &
Q=$!
wait_for() {
    i=0
    while [ $i -lt "$2" ]; do
        grep -qa "$1" "$LOG" 2>/dev/null && return 0
        grep -qa "kernel panic" "$LOG" 2>/dev/null && return 2
        i=$((i+1)); sleep 1
    done
    return 1
}
wait_for "type commands directly" 120 || { echo "没进 shell"; tail -15 "$LOG"; kill $Q; exit 1; }
printf 'install\n' >&3
wait_for "install complete" 300 || { echo "装盘失败"; tail -25 "$LOG"; kill $Q; exit 1; }
printf '/bin/busybox sync\n' >&3
sleep 3
printf 'echo SYNCED\n' >&3
wait_for "SYNCED" 30 || echo "  (警告: 没等到 sync 回显)"
sleep 2
kill $Q 2>/dev/null; wait $Q 2>/dev/null
grep -aE "rootfs copied|install complete" "$LOG" | tail -2 | sed 's/^/    /'

echo ">>> [2] 快照自检(交付序 + 无账号 + 无 install.d)"
LO=$(losetup -f --show -P "$DISK") || exit 1
mkdir -p /mnt/snapchk
mount -o ro ${LO}p2 /mnt/snapchk || { losetup -d "$LO"; exit 1; }
FAIL=""
if [ -f /mnt/snapchk/etc/parlz-auth ]; then
    FAIL="$FAIL 盘上已经有账户表(不该是首启过的盘)"
else
    echo "    OK: 还没有 /etc/parlz-auth(收到盘的人自己设)"
fi
[ -f /mnt/snapchk/install.d ] || [ -d /mnt/snapchk/install.d ] \
    && FAIL="$FAIL 盘上还有 /install.d(会自触发重装)"
# 引导分区的 syslinux.cfg 必须是交付序 —— 从盘上直接读回来验(不是信任输入)
BF=/tmp/snap-boot.img
dd if="$DISK" bs=512 skip=2048 count=131072 of="$BF" status=none
grep -aq 'APPEND console=ttyS0,115200 console=tty0' "$BF" \
    || FAIL="$FAIL 盘里引导分区的 APPEND 不是交付序"
[ "$(mcopy -i "$BF" ::/vmlinuz - 2>/dev/null | md5sum | awk '{print $1}')" \
  = "$(md5sum "$IMG/parlz-bzImage" | awk '{print $1}')" ] \
    || FAIL="$FAIL 盘里的 vmlinuz 不是本轮内核"
umount /mnt/snapchk; losetup -d "$LO"

if [ -n "$FAIL" ]; then
    echo "refresh-shipped-disk: FAIL ->$FAIL"
    exit 1
fi
echo ">>> [3] 同步到交付快照"
cp -f "$DISK" "$OUT" 2>/dev/null || {
    cp -f "$DISK" "$OUT.busy-$(date +%H%M%S)"
    echo "refresh-shipped-disk: $OUT 被占用, 已另存 busy 副本, 本次算失败"; exit 1; }
echo "refresh-shipped-disk: OK -> $OUT($(du -h "$OUT" | awk '{print $1}'))"
echo "    交付序(vga) · 无账户(首启由用户在屏幕上设置) · 内核与本轮一致"
