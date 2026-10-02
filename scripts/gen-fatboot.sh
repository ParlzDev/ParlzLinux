#!/bin/sh
# gen-fatboot.sh - 生成可引导的 64 MiB FAT16 引导分区镜像
# (images/parlz-bootfat.img)+ 元数据头 userland/bootfat.h。
#
# 布局/链路(两条路径共用同一 FAT16 分区):
#   Legacy(SeaBIOS):  MBR(LBA 0, install.c 自写 INT13 AH=0x42 + DAP)
#                     → 读活动分区 VBR(LBA 2048)到 0x7C00 → jmp far
#                     → syslinux VBR 读 /ldlinux.sys → /syslinux.cfg
#                     → LINUX /vmlinuz + APPEND root=/dev/vda2
#   UEFI(OVMF):       识别 0x06/0xEF 分区 → /EFI/BOOT/BOOTX64.EFI
#                     (syslinux.efi PE 桩)→ /EFI/BOOT/syslinux.cfg
#                     → KERNEL /vmlinuz
#
# 镜像由**成熟工具链**生成(不再手写 FAT 字节):
#   dd → mkfs.vfat(-F 16 -s 2) → syslinux --install(VBR+ldlinux.sys)
#   → mcopy 放 vmlinuz / syslinux.cfg / c32 模块
# 理由: 旧版用 Python 逐字节手搓 FAT16, 生成的根目录被内核读成乱码
# (簇/目录区偏移与内核查表不一致), 且 VBR 是 mkfs 的 "not a bootable
# disk" 桩 —— 永远起不来。改用 mkfs.vfat + syslinux 后镜像与内核、
# 与 syslinux 自身都自洽, 并有独立 oracle 可校验(mount + dosfsck)。
#
# 注意: BOOT_SECTORS 改了要同步 userland/install.c 的 BOOT_PART_SIZE_SECTORS。
set -e
# mtools(syslinux --install/mcopy)在名字冲突或只读覆盖时会交互询问;
# 构建必须非交互, 故 stdin 接 /dev/null: 一旦出现询问立即 EOF 报错,
# 而不是静默挂住(踩过: 卡在 "read only, overwrite anyway?")。
exec </dev/null
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# 镜像在 WSL 本地盘生成/校验, 最后只拷一次到 /mnt/f —— 9p 跨盘写 64 MB
# 极慢(dd/mcopy/mount 直接落在 /mnt/f 会卡住数分钟)。
IMG=/home/jgzyes/parlz-bootfat.img
# 落盘路径可覆盖(iso-disk-e2e.sh 用它另存一份串口序的镜像, 不动交付产物)
IMG_OUT=${PARLZ_BOOTFAT_OUT:-/mnt/f/Linux/Parlz/images/parlz-bootfat.img}
OUT=/mnt/f/Linux/Parlz/userland/bootfat.h
VML=/mnt/f/Linux/Parlz/images/parlz-bzImage
KZ=/home/jgzyes/parlz-kernel/arch/x86/boot/bzImage
SYSLINUX_DIR=/usr/lib/syslinux/modules/bios
EFI64_EFI=/usr/lib/SYSLINUX.EFI/efi64/syslinux.efi
EFI64_DIR=/usr/lib/syslinux/modules/efi64
# 64 MiB 分区(= install.c BOOT_PART_SIZE_SECTORS, 64 MiB = 131072 扇区)。
# 64 MiB @ 1 KiB 簇 = ~64760 簇(< FAT16 上限 65524), 容纳约 60 MB vmlinuz。
BOOT_SECTORS=131072
SPC=2                      # 2 扇区/簇(1 KiB 簇), 与内核 fs/fat 兼容

# ---- 依赖 ----
for t in mkfs.vfat syslinux mcopy; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "gen-fatboot: 缺 $t, 先: apt-get install -y dosfstools syslinux mtools" >&2
        exit 1; }
done
[ -f "$VML" ] || { echo "gen-fatboot: 缺 $VML, 先跑 build-kernel.sh" >&2; exit 1; }
[ -f "$SYSLINUX_DIR/ldlinux.c32" ] || {
    echo "gen-fatboot: 缺 $SYSLINUX_DIR/ldlinux.c32(syslinux-common 包)" >&2; exit 1; }

# vmlinuz 必须与内核树 bzImage 同一次构建产物(md5 交叉校验)
if [ -f "$KZ" ]; then
    V1=$(md5sum "$VML" | cut -d' ' -f1)
    V2=$(md5sum "$KZ" | cut -d' ' -f1)
    if [ "$V1" != "$V2" ]; then
        echo "gen-fatboot: images/parlz-bzImage($V1) 与 bzImage($V2) 不一致" >&2
        echo "gen-fatboot: 先跑 build-kernel.sh 再试" >&2
        exit 1
    fi
    echo "gen-fatboot: vmlinuz = $V1 ($(wc -c < "$VML") 字节)"
else
    echo "gen-fatboot: 警告: 找不到 $KZ, 跳过 md5 交叉校验"
fi

# ---- syslinux.cfg: 两条路径统一 ----
# root=/dev/vda2   显式给分区 2(ext2 根, install 写入), 由 busybox body 挂载
# rootdelay=2      等 virtio 分区节点在 devtmpfs 露面(BLKPG 注册不可靠)
# rdinit=/sbin/init PID 1 用 busybox-init(读 /etc/inittab 的 sysinit →
#                  /usr/local/bin/parlz-boot.sh → body: root 探测/挂载/pivot
#                  → login → 交互 shell)。不给 rdinit 时内核默认跑 /init
#                  (init.c, 旧路径), 与当前设计不一致。
# 控制台序与 ISO 共用 scripts/console-cfg.sh(唯一来源):
#   默认 vga 序 console=ttyS0,115200 console=tty0 → /dev/console=tty0,
#   VMware/真机屏幕能看能敲; PARLZ_CONSOLE=serial 给 QEMU -nographic 验收;
#   PARLZ_VGA=0 出无 tty0 的版(发布号后缀 -UN\V)。
. /mnt/f/Linux/Parlz/scripts/console-cfg.sh
CONSOLE_ARGS="$PARLZ_CONSOLE_ARGS"
echo "gen-fatboot: 控制台序 = $PARLZ_CONSOLE_MODE ($CONSOLE_ARGS)"
# PROMPT 1 + TIMEOUT 50: 让 SYSLINUX 自报家门(SAY 必须 ASCII —— 它走 INT10
# 逐字符打到 VGA 文本模式, 那时还没有 UTF-8 控制台)。看不到这两行就是
# MBR/VBR/ldlinux.sys 那一段的问题, 看到了却卡住才是内核/控制台那一段。
CFG_LEGACY="PROMPT 1
TIMEOUT 50
DEFAULT parlz
SAY \">> Parlz: SYSLINUX took over (this line = MBR + VBR + ldlinux.sys OK)\"
SAY \">> Parlz: loading /vmlinuz in 5s (Enter boots now)\"
LABEL parlz
LINUX /vmlinuz
APPEND $CONSOLE_ARGS rdinit=/sbin/init root=/dev/vda2 rootdelay=2
"
CFG_UEFI="PROMPT 1
TIMEOUT 50
DEFAULT parlz
SAY \">> Parlz: SYSLINUX.EFI took over (this line = BOOTX64.EFI + ldlinux.c32 OK)\"
SAY \">> Parlz: loading /vmlinuz in 5s (Enter boots now)\"
LABEL parlz
KERNEL /vmlinuz
APPEND $CONSOLE_ARGS rdinit=/sbin/init root=/dev/vda2 rootdelay=2
"

# ---- 1. 空镜像 + mkfs.vfat(FAT16, 1 KiB 簇) ----
echo "gen-fatboot: mkfs.vfat -F 16 ($((BOOT_SECTORS / 2048)) MiB, $SPC 扇区/簇)..."
rm -f "$IMG" "$IMG.tmp"
dd if=/dev/zero of="$IMG" bs=512 count="$BOOT_SECTORS" status=none
mkfs.vfat -F 16 -n PARLZBOOT -S 512 -s "$SPC" "$IMG" >/dev/null

# ---- 2. syslinux --install: 写真正的 syslinux VBR + /ldlinux.sys ----
# 这一步是磁盘能自举的关键: mkfs.vfat 自己的 VBR 只会打印
# "This is not a bootable disk", syslinux 的 VBR 才会去找 ldlinux.sys。
syslinux --install "$IMG"
echo "gen-fatboot: syslinux VBR + ldlinux.sys 已安装"

# ---- 3. mcopy 放文件 ----
mcopy -o -i "$IMG" "$VML" ::/vmlinuz
printf '%s' "$CFG_LEGACY" > /tmp/parlz-syslinux.cfg
mcopy -o -i "$IMG" /tmp/parlz-syslinux.cfg ::/syslinux.cfg
# ldlinux.sys 与 ldlinux.c32 由 `syslinux --install` 写入(带 hidden+system
# 属性, mdir 需 -a 才可见), 这里不再重复拷 —— 重复拷会触发 mtools 的
# "read only, overwrite anyway?" 交互询问, 非交互构建直接挂死。
# 只补 syslinux 不装、但引导时需要的外部模块。
for m in libcom32.c32 libutil.c32; do
    [ -f "$SYSLINUX_DIR/$m" ] || { echo "gen-fatboot: 缺 $SYSLINUX_DIR/$m" >&2; exit 1; }
    if mdir -a -i "$IMG" "::/$m" >/dev/null 2>&1; then
        echo "gen-fatboot: /$m 已存在, 跳过"
    else
        mcopy -o -i "$IMG" "$SYSLINUX_DIR/$m" "::/$m"
    fi
done

# ---- 4. UEFI(OVMF)路径: 可选, 缺 syslinux.efi 时跳过 ----
if [ -f "$EFI64_EFI" ] && [ -d "$EFI64_DIR" ]; then
    mmd -i "$IMG" ::/EFI ::/EFI/BOOT 2>/dev/null || true
    mcopy -o -i "$IMG" "$EFI64_EFI" ::/EFI/BOOT/BOOTX64.EFI
    for m in config.c32 linux.c32 libcom32.c32 libgpl.c32 libutil.c32 \
             dir.c32 ifcpu64.c32; do
        [ -f "$EFI64_DIR/$m" ] && mcopy -o -i "$IMG" "$EFI64_DIR/$m" "::/EFI/BOOT/$m"
    done
    printf '%s' "$CFG_UEFI" > /tmp/parlz-syslinux-uefi.cfg
    mcopy -o -i "$IMG" /tmp/parlz-syslinux-uefi.cfg ::/EFI/BOOT/syslinux.cfg
    echo "gen-fatboot: UEFI(OVMF) 路径已写入 /EFI/BOOT/"
else
    echo "gen-fatboot: 缺 $EFI64_EFI, 跳过 UEFI(OVMF) 路径(SeaBIOS Legacy 不受影响)"
fi

# ---- 4b. 许可证文本: vmlinuz 就在这个分区上, 文本必须跟着它走 ----
# GPLv2 §3: 分发目标代码时要随附许可证全文(或书面要约)。这个 FAT16 分区
# 装的就是**改过的 Linux 内核**, 单独拿去也能引导 —— 所以许可证不能只放在
# 根分区里, 引导分区自己得有一份。文件名用 8.3 安全的形式。
# 文本的唯一来源是 third_party/licenses/PARLZ-MEDIA-LICENSE.txt(build-userland.sh
# 把同一份放到 rootfs 的 /LICENSE.TXT, ISO 与已安装根因此与这里逐字一致);
# 别在这两处各写一份, 会漂移。
MEDIA_LIC=/mnt/f/Linux/Parlz/third_party/licenses/PARLZ-MEDIA-LICENSE.txt
GPL2=/mnt/f/Linux/Parlz/third_party/licenses/linux-kernel/COPYING
[ -s "$MEDIA_LIC" ] || { echo "gen-fatboot: 缺 $MEDIA_LIC" >&2; exit 1; }
[ -s "$GPL2" ]      || { echo "gen-fatboot: 缺 $GPL2(先跑 scripts/vendor-licenses.sh)" >&2; exit 1; }
mcopy -o -i "$IMG" "$MEDIA_LIC" ::/LICENSE.TXT
mcopy -o -i "$IMG" "$GPL2" ::/COPYING.TXT
echo "gen-fatboot: 引导分区已放 LICENSE.TXT + COPYING.TXT(GPLv2 全文)"

# ---- 5. 独立 oracle 校验 ----
# (a) dosfsck: 文件系统结构必须无错
echo "gen-fatboot: 校验 dosfsck..."
if command -v dosfsck >/dev/null 2>&1; then
    out=$(dosfsck -n "$IMG" 2>&1)
    echo "$out" | head -6
    echo "$out" | grep -qiE "not a|corrupt|error" && {
        echo "gen-fatboot: dosfsck 报告问题, 中止" >&2; exit 1; }
fi
# (b) VBR 必须是 syslinux 的加载器(含 "Boot error"/"Load error"),
#     不是 mkfs.vfat 的 "not a bootable disk" 桩
if strings -n 8 "$IMG" | head -c 4096 | grep -q "not a bootable disk"; then
    echo "gen-fatboot: VBR 是 mkfs 桩(非 syslinux), 中止" >&2; exit 1
fi
strings -n 8 "$IMG" | head -c 4096 | grep -q "Boot error" || {
    echo "gen-fatboot: VBR 未见 syslinux 加载器特征, 中止" >&2; exit 1; }
echo "gen-fatboot: VBR = syslinux 加载器(非 mkfs 桩) OK"

# ---- 6. 挂载复核: 挂到宿主 mount 上, md5 对比 vmlinuz ----
# 这是最强 oracle: 宿主内核自己解析这个 FAT16, 读出的 vmlinuz 必须
# 与内核产物逐字节相同。
MNT=/tmp/parlz-bootimg-mnt
mkdir -p "$MNT"
if mount -o loop,ro "$IMG" "$MNT" 2>/dev/null; then
    echo "gen-fatboot: 挂载内容:"
    ls -la "$MNT" | sed 's/^/    /'
    M1=$(md5sum "$MNT/vmlinuz" 2>/dev/null | cut -d' ' -f1)
    M2=$(md5sum "$VML" | cut -d' ' -f1)
    # 许可证文本必须真在镜像里(mcopy 失败不会让构建停, 只能在挂载后核):
    # COPYING.TXT 是 GPLv2 全文(~18 KB), LICENSE.TXT 是分层授权说明。
    LC_OK=1
    [ -s "$MNT/LICENSE.TXT" ] || { echo "gen-fatboot: 缺 LICENSE.TXT" >&2; LC_OK=0; }
    [ "$(wc -c < "$MNT/COPYING.TXT" 2>/dev/null || echo 0)" -gt 10000 ] \
      || { echo "gen-fatboot: COPYING.TXT 缺失或不是 GPLv2 全文" >&2; LC_OK=0; }
    grep -q "GNU GENERAL PUBLIC LICENSE" "$MNT/COPYING.TXT" 2>/dev/null \
      || { echo "gen-fatboot: COPYING.TXT 内容不是 GPLv2" >&2; LC_OK=0; }
    umount "$MNT" 2>/dev/null || true
    if [ "$M1" != "$M2" ]; then
        echo "gen-fatboot: 镜像内 vmlinuz($M1) != 产物($M2), 中止" >&2
        exit 1
    fi
    [ "$LC_OK" = 1 ] || exit 1
    [ -f "$MNT/ldlinux.sys" ] || true
    echo "gen-fatboot: 镜像内 vmlinuz md5 与产物一致($M1)"
    echo "gen-fatboot: 许可证文本进镜像 OK(LICENSE.TXT + COPYING.TXT/GPLv2)"
else
    echo "gen-fatboot: 警告: 宿主无法 loop 挂载复核(跳过)"
fi

# ---- 7. 元数据头(install 运行时读镜像, 不内嵌字节) ----
NBYTES=$(wc -c < "$IMG")
cat > "$OUT" <<EOF
/* bootfat.h - 引导分区镜像元数据(由 scripts/gen-fatboot.sh 生成)。
 * 镜像本体: images/parlz-bootfat.img(${NBYTES} 字节, 64 MiB FAT16,
 * mkfs.vfat + syslinux --install + mcopy 生成)。
 *   Legacy 路径: syslinux VBR + /ldlinux.sys + /ldlinux.c32 +
 *                /libcom32.c32 + /libutil.c32 + /syslinux.cfg + /vmlinuz
 *   UEFI 路径:   /EFI/BOOT/BOOTX64.EFI + efi64 模块 + syslinux.cfg
 * install.c 运行时读取镜像(ISO / 附加盘 / 文件)整体 pwrite 到 LBA 2048。
 * 刻意不内嵌镜像字节: 镜像含 vmlinuz, vmlinuz 又含 initramfs(内含
 * install 自身), 内嵌会造成自引用膨胀(每轮构建涨一截)。
 * 重新生成: scripts/gen-fatboot.sh */
#ifndef BOOTFAT_H
#define BOOTFAT_H

#define BOOTFAT_IMAGE_BYTES ${NBYTES}u

#endif
EOF
echo "gen-fatboot: 输出 $OUT (元数据头)"
cp -f "$IMG" "$IMG_OUT"
echo "gen-fatboot: 引导镜像 -> $IMG_OUT (${NBYTES} 字节)"
echo "gen-fatboot: OK"
