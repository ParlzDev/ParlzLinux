#!/bin/sh
# 直接读 40 MiB 镜像, 逐字节校验前 512 字节 = C 数组(排除同步问题)
IMG=/home/jgzyes/parlz-fatboot.img
H=/mnt/f/Linux/Parlz/userland/bootfat.h
python3 - "$IMG" "$H" <<'PY'
import re, sys
img, h = sys.argv[1], sys.argv[2]
raw = open(img, "rb").read()
print("img len:", len(raw))
print("img 前 24:", raw[:24].hex(" "))
src = open(h).read()
m = re.search(r"fat16_image\[\d+\] = \{", src)
vals = re.findall(r"0x[0-9a-fA-F]{2}", src[m.end():])
c = bytes(int(v,16) for v in vals)
print("C  前 24:", c[:24].hex(" "))
print("media 差异: img=%d C=%d" % (raw[20], c[20]))
print("完全一致:", raw[:len(c)] == c)
PY
