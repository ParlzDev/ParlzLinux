#!/bin/sh
# gen-cdboot.sh - 从 cdboot.nas 生成 cdboot.bin(512B)+ cdboot.h(C 数组)。
# 用法: sh scripts/gen-cdboot.sh [LBA]
#   LBA = vmlinuz setup 头所在扇区(build-iso.sh 探测后传入)。
#   cdboot.nas 用 CDSLBA 占位;sed 替换成 LBA 十六进制后汇编。
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SRC=/mnt/f/Linux/Parlz/userland
LBA=${1:-4}
cd "$SRC"
LOW16=$(printf "0x%04x" $((LBA & 0xFFFF)))
WORK=/tmp/cdboot-gen.nas
# cdboot.nas 里 CDSLBA 用于 mov cx, CDSLBA(LBA<0x10000,cx=LBA,dx=0)。
sed "s/CDSLBA/$LOW16/g" cdboot.nas > "$WORK"
nasm -f bin "$WORK" -o /tmp/cdboot-raw.bin
python3 - "$SRC" "$LBA" <<'PY'
import sys, os
src, lba = sys.argv[1], int(sys.argv[2])
d = open("/tmp/cdboot-raw.bin","rb").read()
assert len(d) <= 512, f"cdboot too long: {len(d)}"
d = d.ljust(512, b"\x90")
open(os.path.join(src, "cdboot.bin"), "wb").write(d)
lines = ["/* cdboot.h - ISO 引导扇区(512B),由 cdboot.nas 汇编生成 pad 512。",
  f" * INT 13h AH=42(LBA)读 LBA {lba}(vmlinuz setup 头)到 0x7C0,跳 0x7C0。",
  f" * 生成: sh scripts/gen-cdboot.sh {lba} */",
  "#ifndef CDBOOT_H", "#define CDBOOT_H", "",
  "static const unsigned char cd_bootcode[512] = {"]
row = "    "
for b in d:
    s = "0x%02x," % b
    if len(row)+len(s) > 70:
        lines.append(row.rstrip()); row = "    "
    row += s + " "
if row.strip(): lines.append(row.rstrip())
lines += ["};", "", "#endif"]
open(os.path.join(src, "cdboot.h"), "w").write("\n".join(lines))
print(f"cdboot.bin: {len(d)} bytes, 读 LBA {lba}; cdboot.h regenerated")
PY
xxd "$SRC/cdboot.bin" | head -3
