#!/bin/sh
# uefi-e2e.sh - 同一张 hybrid ISO 在 **两种固件**下都要真起到 shell。
#
# 为什么单独一条:Parlz 的交付介质一直是 BIOS 专用(isolinux),固件设成
# UEFI 时(VMware 新建虚机默认就是 UEFI,近十年真机也是)启动项里根本没有它。
# scripts/build-uefi-iso.sh 用 grub-mkrescue 出 BIOS+UEFI 双引导,这条能力
# 一旦被改回单引导不会有人发现 —— 所以判据必须两种固件各跑一遍。
#
# 用例:
#   UEFI : q35 + OVMF pflash 对, -cdrom 引导 → 要看见 GRUB 菜单标题、
#          Parlz 横幅、发布号(带 -F\V,反斜杠不许丢)、Parlz boot ready。
#   BIOS : 默认 SeaBIOS(-machine pc)+ 同一张盘 → 同样四项。
# 两边都带 e1000 + user NAT:body 会跑 `ifc dhcp`,所以还要看见租约落地
# (这条在 UEFI 路径上第一次跑,没有现成判据)。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/uefi-e2e.sh"
#   覆盖镜像: PARLZ_UEFI_ISO=/path/to.iso
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
P=/mnt/f/Linux/Parlz
W=/home/jgzyes/uefi-e2e
rm -rf "$W"; mkdir -p "$W"
# 判据要读串口日志,所以本脚本**自己**出一份串口序的旁支产物(与
# scripts/serial-media.sh 同一思路:跑 -nographic 的验收绝不碰交付那份,
# 交付那份是 vga 序,串口上看不见 shell)。
ISO=${PARLZ_UEFI_ISO:-$W/parlz-hybrid.e2e.iso}
if [ ! -f "$ISO" ]; then
    echo ">>> 生成验收用 hybrid ISO(串口序)"
    PARLZ_CONSOLE=serial PARLZ_UEFI_ISO_OUT="$ISO" \
        sh "$P/scripts/build-uefi-iso.sh" > "$W/build.log" 2>&1 \
        || { echo "build-uefi-iso.sh 失败,末 20 行:"; tail -20 "$W/build.log"; exit 1; }
fi
[ -f "$ISO" ] || { echo "没有 $ISO"; exit 1; }
echo ">>> 被测镜像: $ISO"

# 发布号从仓库根 .parlz-release 取,判据与产物同源(不写死字符串)。
# 取不到就**直接失败**:曾经这里"退到只查 CST+0800"会把整条判据变成假绿 ——
# 反斜杠丢没丢、小版本号对不对,全都查不出来了(见 AGENTS"判据要断终态")。
REL=$(sed -n 's/^PARLZ_RELEASE_ID="\(.*\)"$/\1/p' "$P/.parlz-release" 2>/dev/null | head -1)
[ -n "$REL" ] || { echo "uefi-e2e: 读不到 $P/.parlz-release 的 PARLZ_RELEASE_ID,不能降级判据"; exit 1; }
echo ">>> 期望发布号: $REL"

# boot_case <名字> <qemu 固件参数>
boot_case() {
    _name=$1; _fw=$2
    _log=$W/$_name.log
    echo "--- 用例 $_name ---"
    # shellcheck disable=SC2086
    timeout -k 5 150 qemu-system-x86_64 -m 1024M -nographic -no-reboot \
        $_fw -cdrom "$ISO" -boot d -nic user,model=e1000 \
        </dev/null >"$_log" 2>&1
    echo "    输出行数: $(grep -ac . "$_log")"
    grep -a "GNU GRUB\|menuentry\|Parlz booting\|Parlz 0.1.0\|Parlz release\|boot ready\|ifc: dhcp:\|init: network\|error:" \
        "$_log" | head -8 | sed 's/^/    /'
}

QFW_UEFI="-machine q35 \
  -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/OVMF_CODE_4M.fd,readonly=on \
  -drive if=pflash,format=raw,unit=1,file=$(cp -f /usr/share/OVMF/OVMF_VARS_4M.fd $W/VARS.fd && echo $W/VARS.fd)"
QFW_BIOS="-machine pc"

FAIL=""
for c in UEFI BIOS; do
    case $c in
        UEFI) boot_case uefi "$QFW_UEFI" ;;
        BIOS) boot_case bios "$QFW_BIOS" ;;
    esac
    L=$W/$(echo "$c" | tr 'A-Z' 'a-z').log
    grep -aq "Parlz booting" "$L"        || FAIL="$FAIL $c:GRUB 菜单没出现(引导器没接手)"
    grep -aq "Parlz 0.1.0 on x86_64" "$L" || FAIL="$FAIL $c:内核横幅缺失(卡在引导器之后)"
    grep -qaF "Parlz release $REL" "$L"  || FAIL="$FAIL $c:发布号与 .parlz-release 不同源"
    grep -aq "Parlz boot ready" "$L"     || FAIL="$FAIL $c:没到 boot ready"
    grep -aq "ifc: dhcp: eth0 10.0.2.15/24 gw 10.0.2.2" "$L" \
        || FAIL="$FAIL $c:DHCP 没在这条固件上落地"
    grep -aq "init: network up" "$L"     || FAIL="$FAIL $c:body 没认到网络"
done

if [ -n "$FAIL" ]; then
    echo "uefi-e2e: FAIL ->$FAIL"
    for f in uefi bios; do echo "--- $f 尾 20 行 ---"; tail -20 "$W/$f.log"; done
    exit 1
fi
echo "uefi-e2e: PASS(同一张 ISO 在 UEFI(OVMF) 与 BIOS(SeaBIOS) 下都进到 shell,"
echo "                  发布号同源,DHCP 两条固件都真的配通)"
