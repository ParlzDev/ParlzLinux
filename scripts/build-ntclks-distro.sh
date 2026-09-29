#!/bin/sh
# build-ntclks-distro.sh - 以 NTCLKS 内核为基础, 产出 Parlz 发行版的引导镜像。
#
# 组成(全部由本脚本现做, 不依赖上游的镜像/打包目标 —— 那部分它没有):
#   ① 编译 NTCLKS 六个制品: kernel.sys / kernel.debug / loader.elf / 5 个 .drv / kerneldebug.sys
#   ② installer-root: 一个 FAT 镜像, 放 /usr/lib/leonos/drivers/*.drv
#      (kernel/ntclks/driver_manager.c 就是按这个路径找驱动的)
#   ③ GRUB2(多引导2) El Torito ISO: loader.elf 当 multiboot2 载荷,
#      kernel.sys 与 installer-root 走 module2(模块名 leonos-kernel / leonos-installer-root,
#      与 boot/loader/main.c 的 find_loader_module() 对应)
#   ④ 版本号: NTCLKS 自己的 configs/build-version 改成 Parlz 品牌 + 后缀 -K/RNT;
#      整机发布号 = 仓库 .parlz-release 的 ID 再加 -K/RNT(文件名里 / 换成 -)
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh .../build-ntclks-distro.sh [阶段 [大版本 [小版本]]]"
#   例: sh .../build-ntclks-distro.sh alpha 001 001  ->  ...+alpha.001.001-K/RNT
#   不给参数就用仓库 .parlz-release 的 ID 再拼 -K/RNT
# 产物: images/ntclks/{kernel.sys,loader.elf,*.drv,kerneldebug.sys,installer-root.img,
#                     parlz-ntclks-<ID>-K-RNT.iso}
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export MTOOLS_SKIP_CHECK=1

REPO=/mnt/f/Linux/Parlz
SRC=$REPO/NTCLKS-main
W=/home/jgzyes/parlz-ntclks
O=$W/out/x86_64/release
GEN=$O/generated
OUT=$REPO/images/ntclks
mkdir -p "$OUT"

# ---- 发布号 ----
# 给了三个参数就现场出号(阶段/大/小, 三段写法原样保留, 如 001.001);
# 否则沿用仓库 .parlz-release 的 ID。两种都再接 -K/RNT。
if [ "$#" -ge 3 ]; then
    PARLZ_STAGE=$1; PARLZ_MAJOR=$2; PARLZ_MINOR=$3
    PARLZ_BUILDER=${PARLZ_BUILDER:-jgzyes@parlz.com}
    BT=$(date +%Y%m%d-%H%M%S)
    TZ_STR="CST+0800"
    PARLZ_BUILD_TIME=$BT
    PARLZ_TIMESTAMP=$(date "+%a %b %e %H:%M:%S %Z %Y")
    PARLZ_RELEASE_ID="$TZ_STR+$BT+$PARLZ_BUILDER+$PARLZ_STAGE.$PARLZ_MAJOR.$PARLZ_MINOR"
    cat > "$REPO/.parlz-release-ntclks" <<EOF
# 由 scripts/build-ntclks-distro.sh 生成 —— NTCLKS 发行版的发布号(单一来源)
PARLZ_STAGE="$PARLZ_STAGE"
PARLZ_MAJOR="$PARLZ_MAJOR"
PARLZ_MINOR="$PARLZ_MINOR"
PARLZ_BUILDER="$PARLZ_BUILDER"
PARLZ_BUILD_TZ="$TZ_STR"
PARLZ_BUILD_TIME="$PARLZ_BUILD_TIME"
PARLZ_TIMESTAMP="$PARLZ_TIMESTAMP"
PARLZ_KERNEL=ntclks
PARLZ_KERNEL_VERSION=4.7.2-K/RNT
EOF
elif [ -f "$REPO/.parlz-release" ]; then
    . "$REPO/.parlz-release"
else
    PARLZ_RELEASE_ID="dev.0.1"
fi
ID_TEXT="${PARLZ_RELEASE_ID}-K/RNT"     # 显示用(可含 /)
# 文件名用: / 与 \ 都不能进文件名(Windows 上 \ 还是路径分隔符), 统一换成 -
ID_FILE=$(printf '%s' "${PARLZ_RELEASE_ID}-K-RNT" | tr '/\' '--')
NTV="4.7.2-K/RNT"                       # NTCLKS 侧版本号
echo ">>> 发行号: $ID_TEXT"

echo ">>> [1/5] 同步源码到 WSL 本地盘"
[ -d "$SRC" ] || { echo "缺 $SRC"; exit 1; }
mkdir -p "$W"
( cd "$SRC" && tar -cf - --exclude=out --exclude=cache . ) | ( cd "$W" && tar -xf - )
[ -f "$W/third_party/kconfig-frontends/bootstrap" ] || {
    echo "缺 third_party/kconfig-frontends(子模块没就位)"; exit 1; }
# 该修订把关键字表内联成 kconf_id.c 的手写数组(yconf.y 直接 include),
# 但构建脚本仍按"老修订有 gperf 输入"校验 —— 缺文件时补一个满足校验的桩。
KP=$W/third_party/kconfig-frontends/libs/parser
if [ ! -f "$KP/hconf.gperf" ] && grep -q kconf_id_lookup "$KP/kconf_id.c" 2>/dev/null; then
    printf '%%{\nstatic const struct kconf_id *kconf_id_lookup(register const char *str, register size_t len);\n%%}\n%%%%\n%%%%\n' \
        > "$KP/hconf.gperf"
    echo "    (补 hconf.gperf 桩: 该修订不需要 gperf 输入)"
fi
# 子模块若是 Windows 侧 clone 的, 脚本会是 CRLF(shebang 变 #!/bin/sh\r -> not found)
if head -1 "$W/third_party/kconfig-frontends/bootstrap" | grep -q "$(printf '\r')"; then
    echo "    子模块里有 CRLF, 清理"
    find "$W/third_party/kconfig-frontends" -type f \
        -exec sh -c 'grep -qI . "$1" && sed -i "s/\r$//" "$1"' _ {} \;
    chmod +x "$W/third_party/kconfig-frontends/bootstrap" \
             "$W"/third_party/kconfig-frontends/scripts/*.sh 2>/dev/null || true
fi

echo ">>> [2/5] 打上 Parlz 版本号(configs/build-version)并编译"
# 版本工具要求 release_version 严格 major.minor.patch; 发行标记走 release_suffix
# (为此给 tools/host/version/leonos-version.c 加了这一可选字段, 值会拼进
#  LEONOS_KERNEL_VERSION -> 内核自己报的版本串也带 -K/RNT)
printf '# Parlz 品牌与版本号(由 scripts/build-ntclks-distro.sh 写入)\nkernel_name=parlz-ntclks\nrelease_version=4.7.2\nrelease_suffix=-K/RNT\n' \
    > "$W/configs/build-version"
make -C "$W" O="$O" ARCH=x86_64 PROFILE=release fetch  >/tmp/ntclks-fetch.log 2>&1 \
    || { echo "make fetch 失败:"; tail -20 /tmp/ntclks-fetch.log; exit 1; }
make -C "$W" O="$O" ARCH=x86_64 PROFILE=release all    >/tmp/ntclks-all.log 2>&1 \
    || { echo "内核构建失败:"; tail -30 /tmp/ntclks-all.log; exit 1; }
for f in "$GEN/system/kernel.sys" "$GEN/boot/loader.elf" "$GEN/system/kerneldebug.sys" \
         "$GEN/drivers/serial.drv" "$GEN/drivers/mouse.drv" "$GEN/drivers/e1000.drv" \
         "$GEN/drivers/ac97.drv" "$GEN/drivers/es1371.drv"; do
    [ -f "$f" ] || { echo "缺制品 $f"; exit 1; }
done
echo "    六个制品就绪"

echo ">>> [3/5] installer-root ext2(驱动放 /usr/lib/leonos/drivers/)"
# 内核 storage_mount_ramdisk_root() 只认 ext2(偏移 1080 处幻数 0x53EF)或 FAT32;
# 注释里写明"新介质用 ext2 以保住大小写"(驱动路径大小写敏感)。
IRD=/tmp/parlz-ntclks-root.d
rm -rf "$IRD"
# 1) 目录布局: 按 include/leonos/layout.h 的契约(FHS 式), 内核启动时会逐个初始化
for d in bin sbin lib boot srv tmp media dev leonos \
         usr/bin usr/sbin usr/lib usr/share \
         usr/lib/leonos/drivers usr/lib/leonos/apps usr/lib/leonos/tests \
         usr/share/leonos/resources usr/share/fonts/leonos usr/share/doc/leonos \
         etc/leonos etc/ssl/certs \
         var/lib/leonos var/cache/leonos var/log var/tmp \
         run/leonos; do
    mkdir -p "$IRD/$d"
done
# 2) 用户态: 我们的静态 BusyBox(内核注册了 "Linux x86_64 syscall ABI", 理论上可跑)
BB=/home/jgzyes/parlz-userland/root/sbin/busybox
[ -f "$BB" ] || BB=/home/jgzyes/busybox/busybox
if [ -f "$BB" ]; then
    cp -f "$BB" "$IRD/sbin/busybox"
    cp -f "$BB" "$IRD/sbin/init"      # PID 1: 不用软链, 免得内核的 exec 不吃
    ln -sf ../sbin/busybox "$IRD/bin/sh"
    ln -sf ../sbin/busybox "$IRD/bin/busybox"
    echo "    busybox -> /sbin/init (静态, $(wc -c < "$BB") 字节)"
else
    echo "    警告: 找不到静态 busybox, 发行版将没有用户态"
fi
for d in serial mouse e1000 ac97 es1371; do
    cp -f "$GEN/drivers/$d.drv" "$IRD/usr/lib/leonos/drivers/$d.drv"
done
cp -f "$GEN/system/kernel.sys" "$IRD/leonos/kernel.sys"
printf 'name: ParlzOS (ntclks)\nversion: %s\nkernel: parlz-ntclks %s\nbase: LeonOS 4 kernel (ntclks), Apache-2.0\nbuilder: %s\nbuild-time: %s\n' \
    "$ID_TEXT" "$NTV" "${PARLZ_BUILDER:-jgzyes@parlz.com}" "${PARLZ_BUILD_TIME:-$(date +%Y%m%d-%H%M%S)}" \
    > "$IRD/parlz-release"
IR=/tmp/parlz-ntclks-root.img
rm -f "$IR"
dd if=/dev/zero of="$IR" bs=1M count=64 status=none
mke2fs -q -t ext2 -F -d "$IRD" "$IR"
cp -f "$IR" "$OUT/installer-root.img"

echo ">>> [4/5] GRUB 多引导2 ISO"
ISODIR=/tmp/parlz-ntclks-iso
rm -rf "$ISODIR"; mkdir -p "$ISODIR/boot/grub"
cp -f "$GEN/boot/loader.elf"     "$ISODIR/boot/loader.elf"
cp -f "$GEN/system/kernel.sys"   "$ISODIR/boot/kernel.sys"
cp -f "$GEN/system/kerneldebug.sys" "$ISODIR/boot/kerneldebug.sys"
cp -f "$IR"                      "$ISODIR/boot/installer-root.img"
cat > "$ISODIR/boot/grub/grub.cfg" <<CFG
set timeout=1
set default=0
insmod all_video
insmod multiboot2
insmod serial
# 串口也当终端: -nographic / -serial file: 时能看到 GRUB 自己的输出
# (否则 GRUB 只打 VGA, 冒烟测试抓到一片空白, 分不清是 GRUB 没起来还是多引导失败)
serial --unit=0 --speed=115200
terminal_output serial
terminal_input serial

menuentry "ParlzOS (ntclks) $ID_TEXT" {
    echo "GRUB: loading parlz-ntclks loader.elf (multiboot2)"
    # mode=live: 内核只有看到它(cmdline_has "mode=installer"/"mode=live")才会去
    # 挂 installer-root ramdisk —— 不给的话驱动目录永远"不存在"
    # init=/bin/sh: 直接让 busybox 的 ash 当 PID 1(不经过 inittab), 先把 ABI 跑通
    multiboot2 /boot/loader.elf mode=live init=/bin/sh
    module2 /boot/kernel.sys leonos-kernel
    module2 /boot/installer-root.img leonos-installer-root
    module2 /boot/kerneldebug.sys kerneldebug.sys
    boot
}
CFG
printf 'name: ParlzOS (ntclks)\nversion: %s\nkernel: parlz-ntclks %s\nbase: LeonOS 4 kernel (ntclks), Apache-2.0\n' \
    "$ID_TEXT" "$NTV" > "$ISODIR/parlz-release"
rm -f "$OUT/parlz-ntclks-$ID_FILE.iso"
grub-mkrescue -o "$OUT/parlz-ntclks-$ID_FILE.iso" "$ISODIR" >/tmp/ntclks-grub.log 2>&1 \
    || { echo "grub-mkrescue 失败:"; tail -20 /tmp/ntclks-grub.log; exit 1; }

echo ">>> [5/5] 收制品 + 冒烟引导"
cp -f "$GEN/system/kernel.sys"      "$OUT/kernel.sys"
cp -f "$GEN/boot/loader.elf"        "$OUT/loader.elf"
cp -f "$GEN/system/kernel.debug"    "$OUT/kernel.debug"
cp -f "$GEN/system/kerneldebug.sys" "$OUT/kerneldebug.sys"
for d in serial mouse e1000 ac97 es1371; do
    cp -f "$GEN/drivers/$d.drv" "$OUT/$d.drv"
done
echo "    内核自报版本: $(grep -a 'LEONOS_KERNEL_VERSION ' "$O/include/generated/build_info.h" 2>/dev/null | head -1)"
LOG=/tmp/parlz-ntclks-boot.log; rm -f "$LOG"
# NTCLKS 只支持 **UEFI + SATA**:
#  · UEFI: loader 是 64 位代码, 而 MB2 在 BIOS 下按 32 位保护模式交接 —— 一跳过去就
#    triple fault 复位(实测 monitor 里看到的是复位后的 SeaBIOS 状态)。头部带 EFI64
#    入口标签 + 要 EFI 系统表, 上游就是 UEFI 引导。
#  · SATA: 用 -machine q35(原生 AHCI), 别用默认 pc 那套 IDE。
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
VARS=/tmp/parlz-ntclks-vars.fd
cp -f "$OVMF_VARS" "$VARS"
# -machine q35: 它的磁盘/光驱走 SATA(AHCI) —— NTCLKS 只支持 UEFI + SATA
timeout 60 qemu-system-x86_64 -machine q35 -nic user,model=e1000 -m 1024M -cdrom "$OUT/parlz-ntclks-$ID_FILE.iso" -boot d \
    -drive if=pflash,format=raw,unit=0,file="$OVMF_CODE",readonly=on \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -display none -serial "file:$LOG" -monitor none -no-reboot </dev/null >/dev/null 2>&1 || true
echo "--- 串口抓到 ---"
grep -a "loader\]\|Parlz\|parlz\|ntclks\|LeonOS\|panic\|fault" "$LOG" | head -20 || true
grep -aq "\[loader\]" "$LOG" && echo "SMOKE: loader 起来了(UEFI)" || echo "SMOKE: 串口没看到 loader 输出(见 $LOG)"

echo ">>> [6/6] 收发布产物到 output/"
OUTD=$REPO/output
mkdir -p "$OUTD"
cp -f "$OUT/parlz-ntclks-$ID_FILE.iso" "$OUTD/parlz-ntclks-$ID_FILE.iso"
cat > "$OUTD/parlz-ntclks-$ID_FILE-VERSION.txt" <<EOF
ParlzOS (ntclks) release : $ID_TEXT
stage / major / minor    : ${PARLZ_STAGE:-rc} / ${PARLZ_MAJOR:-0} / ${PARLZ_MINOR:-1}
kernel                   : ntclks $NTV (LeonOS 4 Ring-0 kernel, Apache-2.0)
firmware / storage       : UEFI(OVMF) only; SATA(AHCI, -machine q35) only
builder                  : ${PARLZ_BUILDER:-jgzyes@parlz.com}
build time               : ${PARLZ_TIMESTAMP:-$(date)}
media                    : 引导 ISO(GRUB 多引导2 + installer-root ext2 + 静态 BusyBox)
EOF
cp -f "$OUTD/parlz-ntclks-$ID_FILE-VERSION.txt" "$OUTD/VERSION-ntclks.txt"
(cd "$OUTD" && sha256sum "parlz-ntclks-$ID_FILE.iso" > "parlz-ntclks-$ID_FILE-SHA256SUMS.txt")

echo "=== build-ntclks-distro done ==="
ls -la --block-size=1 "$OUT" | awk 'NR>1{print $5, $9}'
echo "  ISO: $OUT/parlz-ntclks-$ID_FILE.iso"
echo "  运行: wsl -d Ubuntu-24.04 -u root -e bash -c \"sh /mnt/f/Linux/Parlz/scripts/run-ntclks.sh\""
