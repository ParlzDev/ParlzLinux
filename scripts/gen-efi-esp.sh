#!/bin/sh
# gen-efi-esp.sh - 生成 UEFI 引导用的 ESP 镜像(FAT16 + syslinux.efi + 当轮内核)。
#
# 为什么要它: 交付介质一直是 **BIOS 专用**(ISO = El Torito + isolinux,
# 磁盘 = MBR + syslinux VBR)。固件设成 UEFI 时(现在 VMware 新建虚机默认就是
# UEFI, 近十年的真机也是)这张盘**根本起不来** —— 固件的启动项里没有它,
# 强选 "EFI Boot" 就是黑屏。本脚本产出的 ESP 镜像作为 ISO 的**第二条**
# El Torito 引导项(BIOS 那条一个字节不动), UEFI 固件从 ESP 里加载
# /EFI/BOOT/BOOTX64.EFI 接着走。
#
# 三个约束(都是踩过的/照着做的):
#  - ESP 必须是 FAT: UEFI 固件只认 FAT 里的 .efi, 认不了 ISO9660。
#  - syslinux.efi 要 `ldlinux.e64` **与它同目录**(与 isolinux.bin+ldlinux.c32
#    同规则), 少了它就只有 "Boot error" 一行, 看不出原因。
#  - 内核/initramfs 必须在 ESP 里**再放一份**: syslinux.efi 只看得见这块
#    FAT, 看不见 ISO9660 数据区。initramfs 不用再放 —— 内核已经把它嵌在
#    bzImage 里(CONFIG_INITRAMFS_SOURCE), 与 BIOS 那条路完全一样。
#
# 用法: sh gen-efi-esp.sh [输出镜像]
#   PARLZ_APPEND : 与 isolinux.cfg **同一行** APPEND(调用方给, 别让两处各写一份)
#   PARLZ_BZImage: 内核产物(默认 images/parlz-bzImage)
set -eu
P=/mnt/f/Linux/Parlz
OUT=${1:-$P/images/parlz-efi-esp.img}
BZ=${PARLZ_BZImage:-$P/images/parlz-bzImage}
SIZE_MB=${PARLZ_ESP_MB:-40}          # vmlinuz(~28M)+ syslinux.efi+ldlinux.e64(~1.2M)+ 余量
APPEND=${PARLZ_APPEND:-}
[ -n "$APPEND" ] || { echo "gen-efi-esp: 必须给 PARLZ_APPEND(与 isolinux.cfg 同源)"; exit 1; }
[ -f "$BZ" ] || { echo "缺 $BZ(先 build-kernel.sh)"; exit 1; }

E64=/usr/lib/SYSLINUX.EFI/efi64/syslinux.efi
LD64=/usr/lib/syslinux/modules/efi64/ldlinux.e64
[ -f "$E64" ] || { echo "缺 $E64 → apt-get install -y syslinux-efi"; exit 1; }
[ -f "$LD64" ] || { echo "缺 $LD64 → apt-get install -y syslinux-common"; exit 1; }

W=$(mktemp -d /tmp/parlz-esp.XXXXXX)
trap 'rm -rf "$W"' EXIT
IMG=$W/esp.img
# 目标正被 VMware 挂着时删不掉: 先另存 .old-<时分秒> 再写新档(与 ISO 同策略)
if [ -f "$OUT" ]; then
    cp -f "$OUT" "$OUT.old-$(date +%H%M%S)" 2>/dev/null || true
fi
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
# FAT16 + 每簇 2 扇区:与磁盘引导分区那份一致。mkfs.vfat 写的 VBR 是
# "This is not a bootable disk" 桩没关系 —— UEFI 读文件系统里的 .efi,
# 不执行 VBR 引导码(那条只在 BIOS 链上要紧)。
mkfs.vfat -F 16 -s 2 -n PARLZ_EFI "$IMG" >/dev/null

exec </dev/null          # mtools 问交互会挂住脚本
mdir -i "$IMG" ::/ >/dev/null
mmd  -i "$IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$IMG" "$E64"  ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$LD64" ::/EFI/BOOT/ldlinux.e64
mcopy -i "$IMG" "$BZ"   ::/vmlinuz

# syslinux.efi 在**自己所在目录**找 syslinux.cfg(与 isolinux 找根目录那条不同)
cat > "$W/syslinux.cfg" <<CFG
PROMPT 1
TIMEOUT 50
DEFAULT parlz
SAY ">> Parlz: SYSLINUX.EFI took over (this line = UEFI ESP + syslinux.efi OK)"
LABEL parlz
LINUX /vmlinuz
APPEND $APPEND
CFG
mcopy -i "$IMG" "$W/syslinux.cfg" ::/EFI/BOOT/syslinux.cfg

# 三道独立 oracle(文件系统自洽 / 引导件在位 / 内核逐字节等于本轮产物)
dosfsck -a -w "$IMG" >/dev/null 2>&1 || { echo "gen-efi-esp: dosfsck 报错"; exit 1; }
mdir -i "$IMG" -a ::/EFI/BOOT/ | grep -qi 'bootx64' \
    || { echo "gen-efi-esp: ESP 里没有 BOOTX64.EFI"; exit 1; }
mcopy -i "$IMG" ::/vmlinuz "$W/from-esp"
[ "$(md5sum "$W/from-esp" | awk '{print $1}')" = "$(md5sum "$BZ" | awk '{print $1}')" ] \
    || { echo "gen-efi-esp: ESP 里的 vmlinuz 与产物不一致"; exit 1; }

cp -f "$IMG" "$OUT"
echo "gen-efi-esp: OK -> $OUT($(du -h "$OUT" | awk '{print $1}'))"
echo "gen-efi-esp: APPEND = $APPEND"
