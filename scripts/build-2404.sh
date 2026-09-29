#!/bin/bash
# build-2404.sh - 24.04 上的**增量快速重建**: 只同步 login/sh/inittab/boot 脚本,
# 重编 login+sh, 重打 rootfs/initramfs, 编内核, 生成引导镜像, 打 ISO。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash /mnt/f/Linux/Parlz/scripts/build-2404.sh
# 前提: rebuild-wsl-2404.sh 已把 parlz-kernel 源码 + userland root 树就位。
# 输出: images/{parlz-bzImage,parlz-initramfs,parlz-bootfat.img,parlz-install.iso}
#
# ⚠ 改了 userland 里**其它**源码(install.c / mount.c / cpfs.c / busybox /
#   pweb / pm ...)时必须走全量链, 否则新代码不会进 initramfs:
#     sh scripts/build-userland.sh   # 编译全部 userland + 打 initramfs
#     sh scripts/build-kernel.sh     # 嵌 initramfs + 末尾生成引导镜像
#     sh scripts/build-iso.sh        # ISO(含 /boot/fat16.img)
set -e
US=/home/jgzyes/parlz-userland
IMG=/mnt/f/Linux/Parlz/images
K=/home/jgzyes/parlz-kernel
SRC=/mnt/f/Linux/Parlz

# 1) 内核源码树补齐: 标识层 + 补丁(main.c /sbin/init 优先, setup.ld 修复等
#    在 F 盘树里已就位, 直接覆盖到 24.04 树)
echo ">>> [1] 同步内核改动(parlz 标识层 + main.c 双 init)"
cp -f $SRC/linux-7.2.5/init/main.c $K/init/main.c
[ -f $SRC/linux-7.2.5/include/linux/parlz.h ] && cp -f $SRC/linux-7.2.5/include/linux/parlz.h $K/include/linux/parlz.h
[ -f $SRC/linux-7.2.5/arch/x86/kernel/parlz.c ] && cp -f $SRC/linux-7.2.5/arch/x86/kernel/parlz.c $K/arch/x86/kernel/parlz.c
grep -q parlz $K/arch/x86/kernel/Makefile || { echo "F盘树 Makefile 缺 parlz.o 行, 补"; echo 'obj-y += parlz.o' >> $K/arch/x86/kernel/Makefile; }
grep -q parlz_boot_init $K/arch/x86/kernel/head64.c || echo "WARNING: F盘树 head64.c 无 parlz_boot_init hook, 需手补"

# 2) defconfig + 陷阱 2/3 修复
echo ">>> [2] parlz_defconfig + netfilter 关闭 + VIRTIO 直写"
cd $K
make ARCH=x86_64 parlz_defconfig
make olddefconfig ARCH=x86_64 </dev/null >/dev/null
scripts/config -d CONFIG_NETFILTER 2>/dev/null || true
scripts/config -d CONFIG_XTABLES 2>/dev/null || true
# 陷阱 3: VIRTIO 是 select 出来的, 必须直写 .config(AGENTS.md)
grep -q "^CONFIG_VIRTIO=y" .config || echo "CONFIG_VIRTIO=y" >> .config
make olddefconfig ARCH=x86_64 </dev/null >/dev/null
echo "  .config: VIRTIO=$(grep -c '^CONFIG_VIRTIO=y' .config) NETFILTER=$(grep -c '^CONFIG_NETFILTER=y' .config)"

# 3) userland 同步 + 防膨胀占位 + rootfs.cpio.gz
echo ">>> [3] userland 同步 + 打 initramfs"
cd $US
mkdir -p root/etc root/usr/local/bin root/bin root/boot root/sbin
cp -f $SRC/userland/login.c src/login.c
cp -f $SRC/userland/sh.c src/sh.c
cp -f $SRC/userland/etc-inittab root/etc/inittab
cp -f $SRC/userland/parlz-boot.sh root/usr/local/bin/parlz-boot.sh
cp -f $SRC/userland/parlz-boot-body.sh root/usr/local/bin/parlz-boot-body.sh
chmod +x root/usr/local/bin/parlz-boot.sh root/usr/local/bin/parlz-boot-body.sh
rm -f root/boot/vmlinuz.real
rm -f root/boot/vmlinuz; head -c 8192 /dev/zero > root/boot/vmlinuz; chmod 755 root/boot/vmlinuz
# CMake 重编 login/sh(若 build 目录缺, 全量配一次)
if [ ! -d build ]; then
  cmake -S src -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
fi
cmake --build build --target login sh -j"$(nproc)" 2>&1 | tail -2
cp build/bin/login root/bin/login
cp build/bin/sh root/bin/parlz-sh
cd root && find . | LC_ALL=C sort | cpio -o -H newc 2>/dev/null | gzip -9 > $K/rootfs.cpio.gz
echo "  rootfs.cpio.gz = $(du -h $K/rootfs.cpio.gz | cut -f1)"

# 4) 嵌内核 + setup.bin 补丁(陷阱 1)
# 关键: 每次构建前把 CONFIG_INITRAMFS_SOURCE 写回 .config(防上轮
# olddefconfig 把它冲成 ""), 再 non-interactive olddefconfig + 编译。
echo ">>> [4] 嵌 initramfs + 编译内核"
cd $K
grep -q "CONFIG_INITRAMFS_SOURCE=\"$K/rootfs.cpio.gz\"" .config || \
  sed -i "s|CONFIG_INITRAMFS_SOURCE=\"\"|CONFIG_INITRAMFS_SOURCE=\"$K/rootfs.cpio.gz\"|" .config
make ARCH=x86_64 olddefconfig </dev/null >/dev/null 2>&1
echo "  .config: $(grep 'CONFIG_INITRAMFS_SOURCE=' .config)"
make ARCH=x86_64 vmlinux bzImage </dev/null > /tmp/kb24.log 2>&1 || { echo "make FAIL"; tail /tmp/kb24.log; exit 1; }
tail -1 /tmp/kb24.log
rm -f arch/x86/boot/setup.bin
objcopy -O binary arch/x86/boot/setup.elf arch/x86/boot/setup.bin
python3 -c "
d=bytearray(open('arch/x86/boot/setup.bin','rb').read())
d[0]=0xEB; d[1]=0x6A
for i in range(2,0x6C): d[i]=0x90
open('arch/x86/boot/setup.bin','wb').write(bytes(d))
print('  setup.bin patched')
"
make ARCH=x86_64 bzImage </dev/null 2>&1 | tail -1
cp -f arch/x86/boot/bzImage $IMG/parlz-bzImage
cp -f $K/rootfs.cpio.gz $IMG/parlz-initramfs
cp -f $K/rootfs.cpio.gz $US/initramfs.cpio.gz
echo "  bzImage = $(du -h $IMG/parlz-bzImage | cut -f1)"

# 4b) 引导镜像必须跟着内核一起更新, 否则 images/parlz-bootfat.img 里还是
#     上一轮内核 —— 装到磁盘后加载的是旧内核(能起来但版本不对)。
echo ">>> [4b] 生成引导镜像(含当轮内核)"
sh $SRC/scripts/gen-fatboot.sh 2>&1 | tail -3

# 5) ISO
echo ">>> [5] ISO"
sh $SRC/scripts/build-iso.sh 2>&1 | tail -1
echo "=== BUILD_2404_DONE: images/{parlz-bzImage,parlz-initramfs,parlz-bootfat.img,parlz-install.iso} ==="
