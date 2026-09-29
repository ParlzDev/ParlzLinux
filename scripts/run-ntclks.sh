#!/bin/sh
# run-ntclks.sh - 起 QEMU 跑 ParlzOS(ntclks) 发行版 ISO。
#
# NTCLKS 只支持 **UEFI(OVMF) + SATA(AHCI)**:
#   · 固件必须 UEFI: loader 是 64 位代码, MB2 在 BIOS 下按 32 位模式交接, 一跳就
#     triple fault(现象: 屏上/串口什么都不出)。
#   · 机器必须 q35: 它的光驱/磁盘走 SATA(AHCI); 默认 pc 机器是 IDE 那套。
#   PARLZ_BIOS=1  用 BIOS 起(只作对照, 起不来属预期)
#   PARLZ_ESC=1   图形窗口 + 串口都开(需要能用 GTK/SDL 的 WSLg 或 X)
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
OUT=/mnt/f/Linux/Parlz/images/ntclks
ISO=$(ls -t "$OUT"/parlz-ntclks-*.iso 2>/dev/null | head -1)
[ -n "$ISO" ] || { echo "缺 ISO: 先跑 scripts/build-ntclks-distro.sh"; exit 1; }
echo "ISO: $ISO"

if [ "${PARLZ_BIOS:-0}" = "1" ]; then
    echo "(BIOS 引导: 预期起不来, 只作对照)"
    exec qemu-system-x86_64 -machine q35 -nic user,model=e1000 -m "${PARLZ_MEM:-1024M}" -cdrom "$ISO" -boot d -no-reboot -nographic
fi

VARS=/tmp/parlz-ntclks-vars.fd
cp -f /usr/share/OVMF/OVMF_VARS_4M.fd "$VARS" 2>/dev/null || true
if [ "${PARLZ_ESC:-0}" = "1" ]; then
    exec qemu-system-x86_64 -machine q35 -nic user,model=e1000 -m "${PARLZ_MEM:-1024M}" -cdrom "$ISO" -boot d -no-reboot \
        -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/OVMF_CODE_4M.fd,readonly=on \
        -drive if=pflash,format=raw,unit=1,file="$VARS" \
        -serial stdio
fi
exec qemu-system-x86_64 -machine q35 -nic user,model=e1000 -m "${PARLZ_MEM:-1024M}" -cdrom "$ISO" -boot d -no-reboot \
    -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/OVMF_CODE_4M.fd,readonly=on \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -nographic
