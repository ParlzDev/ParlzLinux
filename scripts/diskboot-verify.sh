#!/bin/sh
# diskboot-verify.sh - P0 端到端验证:安装到磁盘 → 从磁盘自启动。
# 阶段 1:SeaBIOS + -kernel + virtio 盘,initramfs 内的 install 写
#   MBR(syslinux mbr.bin)+ FAT16 引导分区(LBA 2048, 40 MiB,
#   含 \EFI\BOOT\BOOTX64.EFI + /vmlinuz + syslinux.cfg)。
#   确认 "install complete"。
# 阶段 2:尝试 OVMF(UEFI)+ IDE 盘(-boot c),OVMF 识别 0xEF/0x06 系统分区,
#   读 /EFI/BOOT/BOOTX64.EFI(syslinux.efi)→ 加载 efi64 模块 →
#   /EFI/BOOT/syslinux.cfg → KERNEL /vmlinuz。
#   注:SeaBIOS(Legacy)在 WSL/Debian QEMU 10.2.1 下读不到 virtio/IDE 盘的
#   MBR 分区("Booting from Hard Disk.." 后静默);OVMF 在本 WSL 环境下
#   对 MBR 分区盘报 "Not Found"(疑为 OVMF 固件 ESP 扫描 + CHS/LBA 兼容)。
#   阶段 2 失败不影响阶段 1 结论;磁盘布局已由 check-disk.py 校验。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/diskboot-verify.sh
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
DISK=/home/jgzyes/parlz-disk.img
SERIAL=/home/jgzyes/parlz-diskboot.log
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC=/usr/share/OVMF/OVMF_VARS_4M.fd
OVMF_VARS=/home/jgzyes/parlz-ovmf-vars.fd

echo "=== 阶段 1: 安装到磁盘 (SeaBIOS + -kernel + virtio 盘) ==="
rm -f "$DISK" "$SERIAL"
truncate -s 512M "$DISK"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -kernel "$IMG/parlz-bzImage" \
  -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 earlycon" \
  -drive file="$DISK",if=virtio,format=raw \
  -nographic -monitor none \
  </dev/null >"$SERIAL" 2>&1 &
QPID=$!
i=0
while [ $i -lt 48 ]; do
  grep -q "install complete" "$SERIAL" 2>/dev/null && break
  i=$((i+1))
  sleep 5
done
if ! grep -q "install complete" "$SERIAL" 2>/dev/null; then
  echo "阶段 1 失败: install complete 未出现,串口日志尾:"
  tail -30 "$SERIAL"
  kill "$QPID" 2>/dev/null || true
  wait "$QPID" 2>/dev/null || true
  exit 1
fi
echo "阶段 1 成功: install complete"
grep -E "FAT16|MBR|LBA 16384|install complete|boot partition|EFI" "$SERIAL" | tail -8
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
sleep 2

echo ""
echo "=== 阶段 2: 从磁盘自启动 (SeaBIOS + IDE 盘 -boot c, 无 kernel/initrd) ==="
# 安装链: SeaBIOS 读 MBR(syslinux mbr.bin, LBA 0) → 跳活动分区 vda1
# VBR(0x7C0) → VBR 实模式 loader(宿主参照提取) → 读 /ldlinux.sys →
# /syslinux.cfg(LINUX /vmlinuz + root=/dev/vda2) → 内核从 vda2 挂载根。
# 成功标记 = 第二遍启动输出 "Parlz"(init 横幅),区别于阶段 1 的 install。
rm -f "$SERIAL"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -drive file="$DISK",if=virtio,format=raw \
  -boot c \
  -nographic -monitor none -no-reboot \
  </dev/null >"$SERIAL" 2>&1 &
QPID=$!
i=0
while [ $i -lt 48 ]; do
  # "Parlz shell" = 磁盘根上 init 进了交互 shell(阶段 2 独有标记,
  # 阶段 1 install 完即退出, 不会走到 shell)
  grep -qa "Parlz shell" "$SERIAL" 2>/dev/null && break
  i=$((i+1))
  sleep 5
done
echo "---- 阶段 2 串口日志(节选) ----"
grep -aE "Booting|Parlz|Linux version|error|ERROR|kernel|root|vda|install complete|Booting from" "$SERIAL" | head -25
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

echo ""
echo "=== 判定 ==="
if grep -qa "Parlz shell" "$SERIAL" 2>/dev/null; then
  echo "PASS: SeaBIOS 磁盘自启动成功, 内核从 vda2 挂载根并进 shell"
  exit 0
else
  echo "FAIL: 阶段 2 未出现 'Parlz shell' 标记, 磁盘自启未成功"
  echo "  串口日志尾:"
  tail -20 "$SERIAL"
  exit 1
fi
