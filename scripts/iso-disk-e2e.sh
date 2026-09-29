#!/bin/sh
# iso-disk-e2e.sh - 真实安装路径端到端: ISO 引导 → 装到磁盘 → 磁盘自启。
#
# 阶段 1(装盘):-cdrom images/parlz-install.e2e.iso(-boot d)+ 空目标盘(512MiB)。
#   这份 ISO 是本脚本现导的**串口序**介质(/dev/console=ttyS0), 交付用的
#   parlz-install.iso 保持 vga 序不动 —— 理由见下面 PARLZ_CONSOLE=serial 那段。
#   El Torito → isolinux → /vmlinuz(内嵌 initramfs)+ APPEND rdinit=/sbin/init
#   → busybox-init → body:
#     · 挂 CD 到 /cdrom(内核 iso9660), 取 /cdrom/boot/fat16.img 当引导镜像源
#     · 跑 /install.d → install:
#         MBR(syslinux mbr.bin 引导码 + 双分区表)
#         引导分区 ← /cdrom/boot/fat16.img 整盘 pwrite 到 LBA 2048
#         ext2 分区 2 + cpfs 拷 rootfs + install-done 标记(LBA 1)
#   成功标记 = "install complete"。
#
# 阶段 2(自启):同一块盘裸引导(SeaBIOS, 无 -kernel/-initrd/-cdrom):
#   MBR → VBR(syslinux) → /ldlinux.sys → /syslinux.cfg → LINUX /vmlinuz
#     + APPEND rdinit=/sbin/init root=/dev/vda2 rootdelay=2
#   → busybox-init → body 挂 /dev/vda2(ext4 驱动挂 ext2)→ pivot_root 换真根
#   → login → shell。
#   成功标记 = "pivot_root OK, now running on installed root"(说明跑的是
#   装到磁盘上的根, 不是 initramfs)且无 kernel panic。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/iso-disk-e2e.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
DISK=/home/jgzyes/parlz-iso-e2e-disk.img
SERIAL=/home/jgzyes/parlz-iso-e2e-serial.log
REPO=/mnt/f/Linux/Parlz

# ★ 本脚本用 QEMU -nographic 跑: 没有能输入的屏幕, /dev/console 必须是串口。
# 交付介质默认是 vga 序(/dev/console=tty0, VMware/真机才能敲键盘), 直接拿它跑
# 这两个阶段, 用户空间输出会全落到看不见的 VGA 上、串口日志空空 → 假失败。
# 所以无条件重导一份**串口序**旁支产物给 e2e 用, 不动 images/ 里的交付文件。
. "$REPO/scripts/serial-media.sh"
E2E_BF=$PARLZ_SERIAL_BF
E2E_ISO=$PARLZ_SERIAL_ISO
echo "=== 准备串口序引导介质(PARLZ_CONSOLE=serial) ==="
parlz_ensure_serial_media force || { echo "串口序介质生成失败"; exit 1; }

for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs" "$E2E_ISO" "$E2E_BF"; do
    [ -f "$f" ] || { echo "缺 $f"; exit 1; }
done

echo "=== 阶段 1: ISO 引导 + 装到磁盘 ==="
rm -f "$DISK" "$SERIAL"
truncate -s 512M "$DISK"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -cdrom "$E2E_ISO" -boot d \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -nographic -monitor none -no-reboot \
  </dev/null >"$SERIAL" 2>&1 &
QPID=$!
i=0
while [ $i -lt 120 ]; do
  grep -qa "install complete" "$SERIAL" 2>/dev/null && break
  grep -qa "kernel panic" "$SERIAL" 2>/dev/null && break
  i=$((i+1)); sleep 5
done
if grep -qa "install complete" "$SERIAL" 2>/dev/null; then
  echo "阶段 1 成功: install complete"
  grep -aE "ISO 介质|引导镜像|boot image source|MBR written|VBR @LBA|mkfs ext2|rootfs copied|install complete" "$SERIAL" | head -14
  # cpfs 失败时 install 只打个警告继续(兜底靠 initramfs), 装出来的根是空的
  # —— 装盘验收必须卡这一行, 否则阶段 2 会以别的形式绕过去。
  grep -qa "rootfs copied" "$SERIAL" 2>/dev/null \
    || { echo "阶段 1 失败: cpfs 没报 rootfs copied"; tail -30 "$SERIAL"; exit 1; }
else
  echo "阶段 1 失败, 串口日志尾:"
  tail -35 "$SERIAL"
  kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
  exit 1
fi
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
sleep 2

echo ""
echo "=== 阶段 2: 从磁盘自启(无 ISO, 无 -kernel) ==="
rm -f "$SERIAL"
FIFO=/home/jgzyes/parlz-iso-e2e.fifo; rm -f "$FIFO"; mkfifo "$FIFO"
# ★ 先以读写方式占住 FIFO: `qemu < $FIFO` 的 open 阻塞到出现写端, 而写端
# 在"等日志"的循环之后 —— 不先占住就死锁(日志一直是空的)。
exec 3<>"$FIFO"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -boot c -nographic -monitor none -no-reboot \
  <"$FIFO" >"$SERIAL" 2>&1 &
QPID=$!
# 装好的盘首启要求设置用户名/密码(syslinux.cfg 里没有 login.skip),
# 不替它喂凭证就永远等不到 shell。
sh /mnt/f/Linux/Parlz/scripts/guest-first-login.sh "$SERIAL" "$FIFO" tester parlz123 300 || true
i=0
while [ $i -lt 60 ]; do
  grep -qa "Parlz shell" "$SERIAL" 2>/dev/null && break
  grep -qa "kernel panic\|not a bootable\|Boot error\|No working init" "$SERIAL" 2>/dev/null && break
  i=$((i+1)); sleep 2
done
echo "---- 阶段 2 串口日志(节选) ----"
grep -aE "Booting|SYSLINUX|Parlz|Linux version|vda|mounted|pivot|EXT4|root device|panic|Boot error" "$SERIAL" | head -25
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true

echo ""
echo "=== 判定 ==="
# 三条都要中: 换根成功 + **已安装根里的 shell 真起来了** + 无 panic。
# (旧版只判 pivot_root OK, 一个空的已安装根照样打印 OK 然后立刻退出 shell。)
if grep -qa "pivot_root OK, now running on installed root" "$SERIAL" 2>/dev/null \
   && grep -qa "Parlz shell" "$SERIAL" 2>/dev/null \
   && ! grep -qa "kernel panic" "$SERIAL" 2>/dev/null; then
  echo "阶段 2 PASS: ISO 安装到磁盘 + 磁盘自启 + 换根后已安装根的 shell 可用"
else
  echo "FAIL: 阶段 2 未进入已安装根的 shell"
  tail -25 "$SERIAL"
  exit 1
fi

echo ""
echo "=== 阶段 3: 宿主复核已安装的根(文件/软链/权限) ==="
sh /mnt/f/Linux/Parlz/scripts/check-installed-root.sh "$DISK" || exit 1

echo ""
echo "iso-disk-e2e: PASS"
exit 0
