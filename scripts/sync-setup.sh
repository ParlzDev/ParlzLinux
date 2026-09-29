#!/bin/sh
# sync-setup.sh - 把 setup.ld 填充值 + setup.bin 修补逻辑同步到
# 任何新 clone 的 Linux 源码树,使 `make bzImage` 一次成功。
# 用法: 在 Linux 源码树根目录运行
#   cd <linux-src> && sh /path/to/sync-setup.sh
set -e
cd "$(cd "$(dirname "$0")/.." && pwd)"

echo ">>> 1. 修正 arch/x86/boot/setup.ld .bstext 填充值 (0xffffffff -> 0x00)"
if grep -q '} =0xffffffff' arch/x86/boot/setup.ld; then
  sed -i 's/} =0xffffffff/} =0x00/' arch/x86/boot/setup.ld
  echo "    done"
else
  echo "    already =0x00"
fi

echo ">>> 2. (跳过 Makefile hook —— 直接由 build-kernel.sh 在 build 后修补 setup.bin)"

echo ">>> 3. 删除旧 setup.bin / bzImage,强制重链"
rm -f arch/x86/boot/setup.bin arch/x86/boot/setup.elf arch/x86/boot/bzImage

echo "=== sync-setup done. 现在运行: make ARCH=x86_64 vmlinux bzImage ==="
