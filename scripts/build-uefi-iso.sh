#!/bin/sh
# build-uefi-iso.sh - 出 **BIOS+UEFI 双引导** ISO(grub-mkrescue 路线)。
#
# 为什么单独一条路: 交付用的 images/parlz-install.iso 是 isolinux(纯 BIOS)。
# 试过给它加第二条 El Torito EFI 引导项(scripts/gen-efi-esp.sh 造 ESP +
# syslinux.efi),xorriso 1.5.6 在 `-as mkisofs` 下 `-b` 与 `-e` **互相顶替**:
# 加完 UEFI 之后 report_el_torito 只剩一条 UEFI —— 等于把 VMware 那条能用的
# BIOS 路弄断了,不能接受。grub-mkrescue 天生就是 hybrid(它内部也是 xorriso,
# 但走 libisofs 的原生 EFI 引导项),所以双引导用 GRUB 出。
#
# 与 BIOS 那条**共用**的东西(不许各说各话):
#  - 同一份内核 images/parlz-bzImage(initramfs 已嵌在里面,不写 initrd 行)
#  - 同一串 APPEND(scripts/console-cfg.sh 出来的控制台序 + rdinit/root)
#  - 同一个 boot/fat16.img(install 装盘时从这里取引导分区镜像)
#
# 输出: $PARLZ_UEFI_ISO_OUT(默认 images/parlz-install-uefi.iso)
#   PARLZ_CONSOLE=serial → 串口序(给 -nographic 验收用),默认 vga(交付)
set -eu
P=/mnt/f/Linux/Parlz
IMG=$P/images
US=${PARLZ_USERLAND:-/home/jgzyes/parlz-userland}
OUT=${PARLZ_UEFI_ISO_OUT:-$IMG/parlz-install-hybrid.iso}
STAGE=${PARLZ_UEFI_STAGE:-/tmp/parlz-uefi-stage}
. "$P/scripts/console-cfg.sh"

[ -f "$IMG/parlz-bzImage" ] || { echo "缺 $IMG/parlz-bzImage,先 build-kernel"; exit 1; }
command -v grub-mkrescue >/dev/null 2>&1 || {
    echo "缺 grub-mkrescue → apt-get install -y grub-mkrescue xorriso"; exit 1; }
[ -d /usr/lib/grub/i386-pc ] && [ -d /usr/lib/grub/x86_64-efi ] || {
    echo "缺 GRUB 模块目录 → apt-get install -y grub-pc-bin grub-efi-amd64-bin"; exit 1; }

APPEND="$PARLZ_CONSOLE_ARGS rdinit=/sbin/init root=/dev/vda2 rootdelay=2"

echo ">>> [1] 准备数据区(rootfs 平铺 + vmlinuz + boot/fat16.img + grub.cfg)"
# 解包目录**不能**在 $STAGE 里,否则整份 rootfs 被打进 ISO 两遍(build-iso.sh 踩过)
rm -rf "$STAGE" /tmp/parlz-uefi.unpack
mkdir -p "$STAGE" /tmp/parlz-uefi.unpack
(cd /tmp/parlz-uefi.unpack && zcat "$US/initramfs.cpio.gz" | cpio -idm 2>/dev/null)
cp -a /tmp/parlz-uefi.unpack/. "$STAGE/" 2>/dev/null || true
rm -rf /tmp/parlz-uefi.unpack
cp -f "$IMG/parlz-bzImage" "$STAGE/vmlinuz"
mkdir -p "$STAGE/boot"
[ -f "$IMG/parlz-bootfat.img" ] && cp -f "$IMG/parlz-bootfat.img" "$STAGE/boot/fat16.img"
mkdir -p "$STAGE/boot/grub"

# GRUB 的控制台:自动化(PARLZ_CONSOLE=serial)时 GRUB 自己的菜单也要走串口,
# 否则"内核起来了但看不见引导器"这段又分不清(与 isolinux 那条 SAY 同目的)。
{
    if [ "$PARLZ_CONSOLE_MODE" = serial ] || [ "$PARLZ_CONSOLE_MODE" = serial-only ]; then
        echo "serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1"
        echo "terminal_input serial"
        echo "terminal_output serial"
    fi
    cat <<CFG
set default=0
set timeout=5
menuentry "Parlz booting (console= order decides who gets keyboard)" {
    linux /vmlinuz $APPEND
}
CFG
} > "$STAGE/boot/grub/grub.cfg"
sed -n 1,12p "$STAGE/boot/grub/grub.cfg" | sed 's/^/    /'

echo ">>> [2] grub-mkrescue(BIOS + UEFI hybrid)"
# 目标可能正被 VMware 当 CD 挂着(9p 上删不掉也改不了名):先写临时名
TMP_ISO="$OUT.tmpbuild"
rm -f "$TMP_ISO"
grub-mkrescue -o "$TMP_ISO" "$STAGE" 2>/tmp/parlz-uefi-mkrescue.log \
    || { echo "grub-mkrescue 失败,末 15 行:"; tail -15 /tmp/parlz-uefi-mkrescue.log; exit 1; }
[ -f "$TMP_ISO" ] || { echo "没产出 $TMP_ISO"; exit 1; }

echo ">>> [3] 验双引导记录"
FAIL=""
file "$TMP_ISO" | grep -aq bootable || FAIL="$FAIL file 没报 bootable"
TOR=$(xorriso -indev "$TMP_ISO" -report_el_torito plain 2>/dev/null || true)
printf '%s\n' "$TOR" | grep -a '^El Torito' | sed 's/^/    /'
printf '%s\n' "$TOR" | grep -aqi 'BIOS'   || FAIL="$FAIL 没有 BIOS 引导项"
printf '%s\n' "$TOR" | grep -aqi 'UEFI'   || FAIL="$FAIL 没有 UEFI 引导项"
SYS=$(xorriso -indev "$TMP_ISO" -report_system_area plain 2>/dev/null || true)
printf '%s\n' "$SYS" | grep -aqi 'EFI boot partition' \
    || FAIL="$FAIL 系统区里没有 EFI 引导分区(OVMF 看不到启动项)"
grep -aqF "linux /vmlinuz $APPEND" "$TMP_ISO" \
    || FAIL="$FAIL ISO 里的 APPEND 与 console-cfg.sh 不一致"
xorriso -indev "$TMP_ISO" -ls / 2>/dev/null | grep -aqi vmlinuz \
    || FAIL="$FAIL 数据区没有 vmlinuz"

# 发布到正式名: mv → 原地覆盖 → 另存 busy-<时分秒> 并以失败退出
if [ -z "$FAIL" ]; then
    if mv -f "$TMP_ISO" "$OUT" 2>/dev/null; then
        :
    elif cat "$TMP_ISO" > "$OUT" 2>/dev/null && \
         [ "$(stat -c%s "$TMP_ISO")" = "$(stat -c%s "$OUT")" ]; then
        echo "    (原地覆盖: $OUT 之前被占用)"
    else
        B=$OUT.busy-$(date +%H%M%S)
        cp -f "$TMP_ISO" "$B"
        echo "build-uefi-iso: $OUT 被占用,已另存 $B,本次算失败"
        exit 1
    fi
fi

if [ -n "$FAIL" ]; then
    echo "build-uefi-iso: FAIL ->$FAIL"
    exit 1
fi
echo "build-uefi-iso: OK -> $OUT($(du -h "$OUT" | awk '{print $1}'))"
echo "    控制台模式: $PARLZ_CONSOLE_MODE · APPEND: $APPEND"
