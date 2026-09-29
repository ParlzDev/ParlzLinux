#!/usr/bin/env python3
import re, sys
src = open("/mnt/f/Linux/Parlz/userland/bootfat.h").read()
m = re.search(r"fat16_image\[\d+\] = \{", src)
arr = src[m.end()+1:]
vals = re.findall(r"0x[0-9a-fA-F]{2}", arr)
vbr = bytes(int(v,16) for v in vals[:512])
# EB 58 90: 入口 0x7C00, jmp 目标 = 0x7C03 + 0x58 = 0x7C5B
# 0x7C5B 对应 VBR 内偏移 0x5B
print("入口指令:", hex(vbr[0]), hex(vbr[1]), hex(vbr[2]))
print("VBR[0x5B:0x60]:", vbr[0x5B:0x60].hex(" "))
print("VBR[0x3E:0x40]:", vbr[0x3E:0x40].hex(" "))
# VBR 引导代码区 0x3E..0x1FD 应全非 0
code = vbr[0x3E:0x200]
print("code 非零字节:", sum(1 for b in code if b != 0), "/", len(code))
# BPB 关键字段对照宿主 mkfs.vfat 参照
import struct
bps = 512 << (vbr[11] - 1)  # bytesPerSector code
spc = vbr[12]
resv = struct.unpack("<H", vbr[13:15])[0]
nfat = vbr[15]
rent = struct.unpack("<H", vbr[16:18])[0]
fs16 = struct.unpack("<H", vbr[28:30])[0]
tot32 = struct.unpack("<I", vbr[32:36])[0]
print(f"BPB: bps={bps} spc={spc} resv={resv} fats={nfat} rent={rent} fs16={fs16} tot32={tot32}")
# 根目录位置
root_rel = resv + nfat * fs16
print(f"根目录在相对 LBA {root_rel}")
# 对照宿主参照(40 MiB FAT16, -s 4 时 fs16=80, 对齐修正后 root_rel=164)
print("宿主参照(8KiB簇): fs16 应为 80, root_rel = 1 + 2*80 = 161; 4KiB簇: fs16=?, ")
print("当前 fs16 =", fs16)
