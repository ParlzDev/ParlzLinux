#!/bin/sh
# build-kernel.sh - 在 WSL Ubuntu 中构建 Parlz 内核 (Linux 7.2.5 源码就地改造)。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/build-kernel.sh
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

K=/home/jgzyes/parlz-kernel
[ -d "$K" ] || { echo "need $K"; exit 1; }
cd "$K"

echo ">>> [1/7] 校验 Parlz 标识层 (头文件 + arch 模块 + head64 挂钩)"
# 标识层已就地打进源码树(parlz.h / parlz.c / head64.c / Makefile),
# 这里只做存在性校验,避免旧版 parlz-patch.sh 重复应用。
[ -f "$K/include/linux/parlz.h" ]      || { echo "MISSING parlz.h"; exit 1; }
[ -f "$K/arch/x86/kernel/parlz.c" ]    || { echo "MISSING parlz.c"; exit 1; }
grep -q "parlz_boot_init" "$K/arch/x86/kernel/head64.c" || { echo "head64.c hook missing"; exit 1; }
grep -q "parlz.o" "$K/arch/x86/kernel/Makefile" || { echo "Makefile obj missing"; exit 1; }
# 标识层的**自有文件**以仓库为准同步进编译副本: /home/jgzyes/parlz-kernel 是一次性
# cp -a 出来的, 改了仓库里的 parlz.h/parlz.c/发布号头却不同步的话, 编出来的内核还是
# 旧横幅(发布号会静默不生效)。只同步 Parlz 自己的文件, 不碰 Linux 上游文件。
R=/mnt/f/Linux/Parlz/linux-7.2.5
for f in include/linux/parlz.h include/linux/parlz-release.h arch/x86/kernel/parlz.c \
         arch/x86/configs/parlz_defconfig; do
    if [ -f "$R/$f" ]; then cp -f "$R/$f" "$K/$f"; fi
done
echo "    identity layer intact (+ 仓库侧标识文件已同步)"

KREL_ARG=""          # 非发布构建就是空串, 版本串走内核自己的默认
REL=/mnt/f/Linux/Parlz/.parlz-release
echo ">>> [1.5/7] 发布号"
if [ -f "$REL" ]; then
    . "$REL"
    [ -n "$PARLZ_RELEASE_ID" ] || { echo "$REL 里没有 PARLZ_RELEASE_ID"; exit 1; }
    # 把发布号原文接进 UTS_RELEASE → /proc/version 与 uname -r 直接带它。
    # UTS_LEN 是 64(含结尾 NUL), 超长会被截断成半个版本号, 所以这里硬卡。
    REL_FULL="7.2.5-$PARLZ_RELEASE_ID"
    [ "${#REL_FULL}" -le 63 ] || {
        echo "发布号太长: UTS_RELEASE=$REL_FULL (${#REL_FULL} > 63)"; exit 1; }
    # CONFIG_LOCALVERSION 装不了反斜杠: kconfig 自己就会把它转义解析坏(实测
    # olddefconfig 之后整个符号变成空串)。所以发布号走 **KERNELRELEASE 命令行变量**:
    # Makefile 里 `ifeq ($(origin KERNELRELEASE),file)` 不成立时, kernel.release 直接
    # 由我们给的值生成, 不再经过 setlocalversion, 也不再经过 kconfig。
    scripts/config -d CONFIG_LOCALVERSION >/dev/null 2>&1 || true
    scripts/config --set-str CONFIG_LOCALVERSION "" >/dev/null 2>&1 || true
    scripts/config -d CONFIG_LOCALVERSION_AUTO
    # 反斜杠会被吃两层: filechk 里的 `echo`(dash 的 builtin 会解一次) + C 字符串字面量
    # 再解一次。实测 8 个进去、1 个出来 —— 层级取决于 /bin/sh 是哪个实现, 所以不写死,
    # 逐层加倍试到"编译出来的 UTS_RELEASE 原样等于发布号"为止。
    KREL_CAND="$REL_FULL"
    PARLZ_KREL=""
    _k=0
    while [ $_k -lt 5 ]; do
        make ARCH=x86_64 KERNELRELEASE="$KREL_CAND" \
            include/config/kernel.release include/generated/utsrelease.h \
            </dev/null >/dev/null 2>&1 || true
        cat > /tmp/parlz-uts-chk.c <<'CEOF'
#include <stdio.h>
#include <generated/utsrelease.h>
int main(void) { puts(UTS_RELEASE); return 0; }
CEOF
        if gcc -I include -I . -o /tmp/parlz-uts-chk /tmp/parlz-uts-chk.c 2>/dev/null \
           && [ "$(/tmp/parlz-uts-chk)" = "$REL_FULL" ]; then
            PARLZ_KREL="$KREL_CAND"
            break
        fi
        KREL_CAND=$(printf '%s' "$KREL_CAND" | sed 's/\\/\\\\/g')   # 反斜杠翻倍再试
        _k=$((_k+1))
    done
    [ -n "$PARLZ_KREL" ] || {
        echo "转义试了 $_k 层仍对不上: uname -r 显示不出 '$REL_FULL'(发布号里的反斜杠被吃光)"
        exit 1; }
    KREL_ARG="KERNELRELEASE=$PARLZ_KREL"
    export KREL_ARG
    echo "    KERNELRELEASE 传参($_k 层加倍, 反斜杠 $(printf '%s' "$PARLZ_KREL" | tr -cd '\\' | wc -c) 个) -> uname -r = $REL_FULL"
    # linux_banner 里的 "(构建人@构建主机)" 与末尾时间戳也按发布号来
    export KBUILD_BUILD_USER="${PARLZ_BUILDER%@*}"
    export KBUILD_BUILD_HOST="${PARLZ_BUILDER#*@}"
    if [ -n "${PARLZ_TIMESTAMP:-}" ]; then
        export KBUILD_BUILD_TIMESTAMP="$PARLZ_TIMESTAMP"
    fi
    echo "    发布号 $PARLZ_RELEASE_ID"
    echo "    UTS_RELEASE -> $REL_FULL (${#REL_FULL}/63)"
    echo "    banner -> ($KBUILD_BUILD_USER@$KBUILD_BUILD_HOST) ${KBUILD_BUILD_TIMESTAMP:-<宿主当前时间>}"
else
    echo "    无 .parlz-release —— 非发布构建, 用源码树里的默认发布号(parlz-release.h)"
fi

echo ">>> [2/7] 生成 Parlz defconfig"
make ARCH=x86_64 $KREL_ARG olddefconfig </dev/null >/dev/null   # keep current .config (new symbols INITRAMFS_ROOT_UID etc. default)
make ARCH=x86_64 $KREL_ARG olddefconfig </dev/null >/dev/null

echo ">>> [3/7] 关闭 GCC 15 下出问题的 netfilter/xtables"
scripts/config -d CONFIG_NETFILTER 2>/dev/null || true
scripts/config -d CONFIG_XTABLES 2>/dev/null || true
make ARCH=x86_64 $KREL_ARG olddefconfig </dev/null >/dev/null

echo ">>> [4/7] 修复 setup.ld .bstext 段填充值 (=0xffffffff -> =0x00)"
# GCC 15 + binutils 2.46 下,header.o 的 .bstext 段内容为空,
# 链接器按 setup.ld 的 =0xffffffff 填充,导致 setup.bin 前 495 字节
# 全 0xFF,SeaBIOS 跳 0x7C00 后执行 0xFF 卡死。改为 =0x00 修复。
if grep -q '} =0xffffffff' arch/x86/boot/setup.ld; then
  sed -i 's/} =0xffffffff/} =0x00/' arch/x86/boot/setup.ld
  echo "    setup.ld .bstext fill: 0xffffffff -> 0x00"
fi
grep -n '} =' arch/x86/boot/setup.ld

echo ">>> [5/7] 编译 vmlinux + bzImage(嵌入 initramfs,磁盘启动自举)"
# 嵌入 initramfs:磁盘启动走 MBR → vmlinuz,无 QEMU -initrd,
# 内核必须自带 rootfs。CONFIG_INITRAMFS_SOURCE 指向 gzip 的 cpio,
# build-kernel 在 userland 构建后运行;若 root 布局尚未生成,
# 先跑 build-userland(它产出 /home/jgzyes/parlz-userland/root)。
ROOTFS=/home/jgzyes/parlz-userland/root
if [ ! -d "$ROOTFS" ] || [ -z "$(ls -A $ROOTFS 2>/dev/null)" ]; then
  echo "    rootfs 缺失,先构建 userland..."
  sh /mnt/f/Linux/Parlz/scripts/build-userland.sh || exit 1
fi
# 把 rootfs 打成 gzip cpio,放到 kernel 树
(cd "$ROOTFS" && find . | LC_ALL=C sort | cpio -o -H newc 2>/dev/null | gzip -9) \
  > "$K/rootfs.cpio.gz"
echo "    嵌入 initramfs: $(ls -la $K/rootfs.cpio.gz | awk '{print $5}') bytes"
make ARCH=x86_64 $KREL_ARG CONFIG_INITRAMFS_SOURCE="$K/rootfs.cpio.gz" \
     vmlinux bzImage 2>&1 | tail -5

echo ">>> [6/7] 修补 setup.bin 实模式入口 (.bstext 段空,注入 jmp start_of_setup)"
rm -f arch/x86/boot/setup.bin
objcopy -O binary arch/x86/boot/setup.elf arch/x86/boot/setup.bin
# 先重建 bzImage(用未修补的 setup.bin),再删 setup.bin 重新 objcopy + 修补,
# 最后一次重建让 bzImage 内嵌修补后的 setup.bin。
make ARCH=x86_64 $KREL_ARG bzImage >/dev/null 2>&1
rm -f arch/x86/boot/setup.bin
objcopy -O binary arch/x86/boot/setup.elf arch/x86/boot/setup.bin
python3 -c "
data = open('arch/x86/boot/setup.bin','r+b').read()
data = bytearray(data)
data[0] = 0xEB; data[1] = 0x6A        # jmp +0x6A -> 0x26C start_of_setup
for i in range(2, 0x6C): data[i] = 0x90  # NOP fill
open('arch/x86/boot/setup.bin','wb').write(data)
print('    setup.bin patched: EB 6A at 0x0, NOP 0x2..0x6B')
"
make ARCH=x86_64 $KREL_ARG bzImage 2>&1 | tail -2
echo "    bzImage 首 8B: $(xxd -l 8 arch/x86/boot/bzImage | head -1 | cut -d' ' -f2-)"

echo ">>> [6.5/7] 硬校验发布号真进了 UTS_RELEASE"
# 一律 grep -aF(定值串): 用 BRE 的话 "\V" 会被当成 "V", 反斜杠丢光了也照样"通过"
# —— 这个自欺踩过一次(uname -r 打成 -FV, 五处判据里三处却是绿的)。
if [ -n "${REL_FULL:-}" ]; then
    if grep -aqF "Linux version $REL_FULL" "$K/vmlinux"; then
        echo "    vmlinux 的 linux_banner 逐字含: Linux version $REL_FULL"
    else
        echo "FAIL: vmlinux 里没有 'Linux version $REL_FULL'"
        strings -a "$K/vmlinux" | grep -a -m2 'Linux version ' | sed 's/^/    实际: /'
        exit 1
    fi
fi

echo ">>> [7/7] 拷贝产物到 images/ + 同步 root/boot/vmlinuz"
mkdir -p /mnt/f/Linux/Parlz/images
cp arch/x86/boot/bzImage /mnt/f/Linux/Parlz/images/parlz-bzImage
# rootfs 的 /boot/vmlinuz 必须与内核同一次构建产物,否则
# userland(rootfs 布局)早于 kernel(rootfs.cpio.gz 打包)时,
# 磁盘启动写入 LBA 16384 的会是旧内核,启动卡死。
cp -f arch/x86/boot/bzImage /home/jgzyes/parlz-userland/root/boot/vmlinuz
echo "=== kernel build done: /mnt/f/Linux/Parlz/images/parlz-bzImage ==="
ls -la /mnt/f/Linux/Parlz/images/parlz-bzImage /home/jgzyes/parlz-userland/root/boot/vmlinuz
md5sum arch/x86/boot/bzImage /home/jgzyes/parlz-userland/root/boot/vmlinuz

# 防自引用膨胀兜底: 上面把 bzImage 同步进 root/boot/vmlinuz, 若之后
# 再跑 build-kernel(它会把整个 root 打成 rootfs.cpio.gz 嵌入内核),
# root/boot/vmlinuz 会把整个 bzImage 烘进 initramfs → 下个 build-userland
# 又拷回 → 60M→120M 循环直到 do_populate_rootfs 写挂。
# 这里立刻把它降回 8KB 占位: gen-fatboot 读的是 arch/.../bzImage
# (同 md5, 真 vmlinuz 只进 FAT 引导分区), 内核根不需要 root/boot/vmlinuz。
rm -f /home/jgzyes/parlz-userland/root/boot/vmlinuz
head -c 8192 /dev/zero > /home/jgzyes/parlz-userland/root/boot/vmlinuz
chmod 755 /home/jgzyes/parlz-userland/root/boot/vmlinuz
echo "=== root/boot/vmlinuz 已降为 8KB 占位(防 initramfs 自引用膨胀) ==="

echo ">>> [8/7] 生成引导镜像 images/parlz-bootfat.img(内含当轮内核)"
# 必须在内核产物就绪后跑: 镜像里的 /vmlinuz 要= 本轮 bzImage,否则磁盘
# 自启加载的是旧内核。install 运行时读这个镜像(不再内嵌,见 install.c)。
sh /mnt/f/Linux/Parlz/scripts/gen-fatboot.sh || {
  echo "gen-fatboot 失败: 引导镜像未更新"; exit 1; }
ls -la /mnt/f/Linux/Parlz/images/parlz-bootfat.img
