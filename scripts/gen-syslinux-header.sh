#!/bin/sh
# gen-syslinux-header.sh - 把 /usr/lib/syslinux 的现成组件转成
# userland/syslinux.h(C 数组,install.c 静态链接用):
#   syslinux_mbr    : mbr.bin(440B),写磁盘 LBA 0
#   syslinux_ldlinux: ldlinux.c32(118420B),拷进 boot 分区 /ldlinux.sys
# 源二进制存 userland/syslinux-mbr.bin、syslinux-ldlinux.c32
# (首次由 build-userland.sh 从 /usr/lib/syslinux 拷入)。
# 重新生成: scripts/gen-syslinux-header.sh
set -e
cd "$(dirname "$0")/../userland"

SYSLINUX_DIR=/usr/lib/syslinux
[ -f "$SYSLINUX_DIR/mbr/mbr.bin" ] || {
    echo "gen-syslinux-header: $SYSLINUX_DIR/mbr/mbr.bin 缺失"; exit 1; }
[ -f "$SYSLINUX_DIR/modules/bios/ldlinux.c32" ] || {
    echo "gen-syslinux-header: $SYSLINUX_DIR/modules/bios/ldlinux.c32 缺失"; exit 1; }
cp -f "$SYSLINUX_DIR/mbr/mbr.bin" syslinux-mbr.bin
cp -f "$SYSLINUX_DIR/modules/bios/ldlinux.c32" syslinux-ldlinux.c32

python3 - "$@" <<'PYEOF'
import sys, os

def emit_array(fname, arrname, f):
    data = open(fname, 'rb').read()
    f.write("static const unsigned char %s[%d] = {\n" % (arrname, len(data)))
    for i in range(0, len(data), 12):
        f.write("    " + ",".join("0x%02x" % b for b in data[i:i + 12]) + ",\n")
    f.write("};\n")

with open("syslinux.h", "w") as f:
    f.write("""/* syslinux.h - 现成 syslinux 引导组件(来自 /usr/lib/syslinux),自动生成。
 * syslinux_mbr: mbr.bin(440B),写磁盘 LBA 0;INT 13h/LBA 读分区表,
 *   找 FAT 分区读 /ldlinux.sys(syslinux BIOS 引导器)。
 * syslinux_ldlinux: ldlinux.c32(118420B),由 install 写进 boot 分区
 *   /ldlinux.sys;ldlinux.sys 读 syslinux.cfg 执行 linux /vmlinuz。
 * 源文件: userland/syslinux-mbr.bin、syslinux-ldlinux.c32
 * 重新生成: scripts/gen-syslinux-header.sh
 */
#ifndef SYSLINUX_H
#define SYSLINUX_H

""")
    emit_array("syslinux-mbr.bin", "syslinux_mbr", f)
    f.write("\n")
    emit_array("syslinux-ldlinux.c32", "syslinux_ldlinux", f)
    f.write("\n#endif\n")

print("syslinux.h: %d bytes" % os.path.getsize("syslinux.h"))
PYEOF
