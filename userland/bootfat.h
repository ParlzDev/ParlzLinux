/* bootfat.h - 引导分区镜像元数据(由 scripts/gen-fatboot.sh 生成)。
 * 镜像本体: images/parlz-bootfat.img(67108864 字节, 64 MiB FAT16,
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

#define BOOTFAT_IMAGE_BYTES 67108864u

#endif
