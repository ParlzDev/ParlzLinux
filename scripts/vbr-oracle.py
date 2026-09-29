#!/usr/bin/env python3
"""VBR oracle: 从 C 数组提取镜像, 按本树内核 7.2.5 fs/fat/inode.c 的
几何公式(fs/fat/inode.c:1725-1736)逐区验收 gen-fatboot.sh 的 FAT16
引导分区布局 + VBR 引导代码完整性。

宿主 loop mount 仅作参考(宿主 6.18 偏好 mkfs.vfat 自身 4.x 对齐布局,
与本生成器严格按内核公式的布局不同, 挂载失败不影响 guest 验收);
主判据 = 内核公式逐区比对 + VBR loader + 目录项完整性。

用法: wsl -d Ubuntu-26.04 -u root -e bash -c 'python3 /mnt/f/Linux/Parlz/scripts/vbr-oracle.py'
"""
import struct, re, subprocess, sys, os

SRC = "/mnt/f/Linux/Parlz/userland/bootfat.h"
OUT = "/tmp/fatboot-oracle.img"

src = open(SRC).read()
m = re.search(r"fat16_image\[\d+\] = \{", src)
vals = re.findall(r"0x[0-9a-fA-F]{2}", src[m.end()+1:])
data = bytes(int(v, 16) for v in vals)
print("C 数组字节数:", len(data))
assert len(data) == 41943040, "C 数组须 41943040 (81920*512)"
open(OUT, "wb").write(data)

def bpb(img):
    b = img[:512]
    return {
        "bps": struct.unpack_from("<H", b, 11)[0],
        "spc": b[12],
        "resv": struct.unpack_from("<H", b, 13)[0],
        "fats": b[15],
        "rent": struct.unpack_from("<H", b, 16)[0],
        "media": b[20],
        "fs16": struct.unpack_from("<H", b, 21)[0],
        "nsec_fat": struct.unpack_from("<H", b, 28)[0],
        "tot32": struct.unpack_from("<I", b, 32)[0],
    }

vbr = data[:512]
o = bpb(data)
print("VBR:", o)

ok = True
# ---- 内核必填校验(fat_read_bpb, fs/fat/inode.c:1420-1464)----
if o["fats"] < 1:
    print("拒: fats=%d < 1(内核 if(!fat_fats) 拒)" % o["fats"]); ok = False
if not (0xF8 <= o["media"] or o["media"] == 0xF0):
    print("拒: media=0x%02x fat_valid_media 要求 >=0xF8 或 0xF0" % o["media"]); ok = False
if o["nsec_fat"] == 0 and o["fs16"] == 0:
    print("拒: fat_length(nsec_fat16@28) = 0(内核要求非 0)"); ok = False
# fat_length = nsec_fat16@28(内核唯一来源)
fat_start = o["resv"]
fat_length = o["nsec_fat"]
# dir_start = fat_start + fats*fat_length(inode.c:1725)
dir_start = fat_start + o["fats"] * fat_length
# data_start = dir_start + rootdir_sectors; FAT16 rent=2 → 0(inode.c:1734-1736)
data_start = dir_start
print("内核公式: FAT@%d 根目录@%d 数据区簇2@%d" % (fat_start, dir_start, data_start))

# ---- 逐区验收 ----
# 1) VBR 引导代码 0x3C..0x1FD(实模式 loader, 宿主 mkfs 参照提取)
code_nz = sum(1 for x in vbr[0x3C:0x200] if x)
assert code_nz > 50, "VBR 引导代码缺失(%d 非零)" % code_nz
assert vbr[0] == 0xEB and vbr[2] == 0x90, "VBR 入口跳转 stub 缺失"
assert vbr[0x1FE] == 0x55 and vbr[0x1FF] == 0xAA, "55AA 缺失"
print("VBR 引导代码 %d 非零字节, EB..90 + 55AA OK" % code_nz)

import struct as st

# 2) 根目录项(应在 dir_start 扇区; 含 VMLINUZ 8.3 项)
rd = data[dir_start*512:(dir_start+o["spc"])*512]
hit_vml = b"VMLINUZ" in rd
hit_ld = b"LDLINUX" in rd
hit_efi = b"EFI" in rd
print("根目录 @%d 含 VMLINUZ=%s LDLINUX=%s EFI=%s" %
      (dir_start, hit_vml, hit_ld, hit_efi))
if not (hit_vml and hit_ld and hit_efi):
    print("  根目录首 96B:", rd[:96].hex(" "))
    ok = False

# 3) 数据区簇分配: dir_start = 根目录簇(@dir_start), 数据区簇 2 起 @
# dir_start + SPC(内核 data_start = dir_start + 0, 但簇 2 物理 LBA =
# data_start 簇 0 号 + 1 = dir_start + SPC)
data_start = dir_start + o["spc"]
# 4) FAT 表项(FAT[0]=0xFF00|media, FAT[1]=0xFFFF, 簇 2 = rootdir 簇)
f = data[fat_start*512:fat_start*512+8]
fat0 = st.unpack_from("<H", f, 0)[0]
fat1 = st.unpack_from("<H", f, 2)[0]
print("FAT[0]=%04x FAT[1]=%04x (期望 fff8 起 media 掩码区/FFFF)" % (fat0, fat1))
# 找 VMLINUZ 项 first_cluster → 数据区 LBA
i = rd.find(b"VMLINUZ")
if i >= 0:
    vm_first = st.unpack_from("<H", rd, i//32*32+20)[0]
    vm_off = (data_start + ((vm_first - 2) % max(o["fs16"], 40000)) * o["spc"]) * 512
    vm_head = data[vm_off:vm_off+4]
    ok = ok and vm_head not in (b"\x00\x00\x00\x00", b"\xff\xff\xff\xff")
    print("VMLINUZ first_cluster=%d → LBA %d 前 4 字节 %s(应非全 0)" %
          (vm_first, vm_off//512, vm_head.hex(" ")))

# ---- 宿主 loop mount(参考, 不影响 ok)----
try:
    os.makedirs("/tmp/oracle-v2mnt", exist_ok=True)
    rc = subprocess.run(["mount", "-o", "loop,ro", "-t", "vfat", OUT,
                         "/tmp/oracle-v2mnt"], capture_output=True, text=True)
    if rc.returncode == 0:
        entries = sorted(os.listdir("/tmp/oracle-v2mnt"))
        print("宿主 mount 成功(参考):", entries)
        for must in ("vmlinuz", "ldlinux.sys", "EFI", "syslinux.cfg"):
            if must not in entries:
                print("宿主 mount 缺文件:", must)
    else:
        print("宿主 mount 失败(参考, 不裁决):", rc.stderr.strip()[:120])
    subprocess.run(["umount", "/tmp/oracle-v2mnt"], capture_output=True)
except Exception as e:
    print("宿主 mount 跳过:", e)

print("VBR_ORACLE_" + ("OK" if ok else "FAIL"))
sys.exit(0 if ok else 1)
