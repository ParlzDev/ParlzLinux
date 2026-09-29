#!/bin/sh
# 定位宿主 6.18 内核拒挂的原因: 逐字段比对 our.img vs ref2.img VBR
rm -f /tmp/our.img
python3 - <<PY
import re, struct
src = open("/mnt/f/Linux/Parlz/userland/bootfat.h").read()
m = re.search(r"fat16_image\[\d+\] = \{", src)
vals = re.findall(r"0x[0-9a-fA-F]{2}", src[m.end():])
open("/tmp/our.img","wb").write(bytes(int(v,16) for v in vals))
PY
echo "=== VBR 逐字段 our vs ref2(宿主参照, 可挂载) ==="
python3 - <<PY
import struct
o = open("/tmp/our.img","rb").read(512)
r = open("/tmp/ref2.img","rb").read(512)
f16 = lambda b,off: struct.unpack_from("<H", b, off)[0]
f32 = lambda b,off: struct.unpack_from("<I", b, off)[0]
rows = [
    ("OEM(3..10)", o[3:11], r[3:11]),
    ("bps_code@11", o[11], r[11]),
    ("spc@12", o[12], r[12]),
    ("resv@13", f16(o,13), f16(r,13)),
    ("fats@15", o[15], r[15]),
    ("rent@16", f16(o,16), f16(r,16)),
    ("tot16@18", f16(o,18), f16(r,18)),
    ("media@20", o[20], r[20]),
    ("fs16@21", f16(o,21), f16(r,21)),
    ("spt@24", f16(o,24), f16(r,24)),
    ("heads@26", o[26], r[26]),
    ("nsec_fat16@28", f16(o,28), f16(r,28)),
    ("tot32@32", f32(o,32), f32(r,32)),
    ("volSer@36", f32(o,36), f32(r,36)),
    ("extLbl@40", o[40:44], r[40:44]),
    ("driveNum@48", o[48], r[48]),
    ("bsig@49", o[49], r[49]),
    ("label@51", o[51:62], r[51:62]),
    ("fstype@62", o[62:70], r[62:70]),
    ("55AA", o[0x1FE:0x200], r[0x1FE:0x200]),
]
for name, ov, rv in rows:
    mark = "  " if ov == rv else ">>"
    print("%s %-14s ours=%s ref=%s" % (mark, name, ov, rv))
PY
