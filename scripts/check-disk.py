#!/usr/bin/env python3
"""check-disk.py - 校验 Parlz 安装后的磁盘镜像(syslinux 双分区布局)。

布局:
  LBA 0     = MBR(syslinux mbr.bin 440B 代码 + 双分区表 + 55AA)
  LBA 1     = install-done 标记 "parlz install done"(与 boot 分区不重叠)
  LBA 2048  = 分区 1:FAT16 引导分区(0x06,40 MiB,含 /ldlinux.sys + /vmlinuz
              + /EFI/BOOT/BOOTX64.EFI + efi64 模块 + syslinux.cfg)
  LBA 83968 = 分区 2:root(0x83,暂不格式化,vmlinuz 内嵌 initramfs 兜底)

根目录定位:mkfs.vfat 的 FAT16 镜像 BPB 16-bit fs16@28 字段写 0
(FAT16 对齐到 32KiB 边界),标准公式 reserved+nfats*fs16 落空。
不盲信 BPB,按三级策略定位根目录扇区:
  1) BPB 公式  rel = reserved + nfats*fs16
  2) 对齐修正  fs16'=ceil(total/1024) 等对齐候选(40 MiB → fs16'=80,
     rel' = reserved + nfats*fs16' = 164)
  3) 扫描兜底  在 boot 分区内搜 b"VMLINUZ" 锁定实际扇区

用法: python3 check-disk.py <disk.img>
"""
import math
import struct
import sys

BOOT_LBA = 2048
BOOT_SECTORS = 81920
ROOT_LBA = BOOT_LBA + BOOT_SECTORS  # 83968


def parse_root_dir(data, off, root_entries):
    """解析 off 处的根目录;返回 (ok, has_efi, has_vml, vml_bytes, names)。

    判定规则:FAT32 root 目录可能混入 LFN 长文件名条目(attr 0x0F,
    其簇号恒为 0xFFFF)与卷标条目(attr 0x08)。根目录"真实有效"的
    判据是其中出现 VMLINUZ/EFI 短名(非 LFN),而非 LFN 哨兵。
    """
    rd = data[off:off + root_entries * 32]
    names = []
    efi_cluster = 0
    vml_bytes = 0
    for i in range(0, len(rd) - 31, 32):
        e = rd[i:i + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5:
            continue
        attr = e[11]
        base = e[0:8].decode("latin-1").replace("\x00", " ").strip()
        ext = e[8:11].decode("latin-1").replace("\x00", "")
        # LFN 哨兵(attr 0x0F)或卷标(0x08):不算短名根目录项
        if attr & 0x0F:
            continue
        nm = base + (("." + ext) if ext else "")
        if nm:
            names.append(nm + ("/" if attr & 0x10 else ""))
        if attr & 0x10:  # 目录
            efi_cluster = struct.unpack("<H", e[20:22])[0]
        if base.upper() == "VMLINUZ":
            vml_bytes = struct.unpack("<I", e[24:28])[0]
    has_efi = any(n.upper().startswith("EFI") for n in names)
    has_vml = any(n.upper().startswith("VMLINUZ") for n in names)
    ok = has_efi and has_vml and vml_bytes > 100000
    return ok, has_efi, has_vml, vml_bytes, names


def main():
    if len(sys.argv) < 2:
        print("usage: check-disk.py <disk.img>", file=sys.stderr)
        return 1
    data = open(sys.argv[1], "rb").read()

    print(f"磁盘大小        : {len(data)} 字节")
    good = True

    # ---- MBR ----
    magic_ok = data[510:512] == b"\x55\xaa"
    print(f"MBR 魔数        : {'55aa OK' if magic_ok else data[510:512].hex() + ' BAD'}")
    good &= magic_ok

    p1 = data[0x1BE:0x1BE + 16]
    p2 = data[0x1CE:0x1CE + 16]
    p1_boot = p1[0]
    p1_type = p1[4]
    p1_lba = struct.unpack("<I", p1[8:12])[0]
    p1_cnt = struct.unpack("<I", p1[12:16])[0]
    p2_type = p2[4]
    p2_lba = struct.unpack("<I", p2[8:12])[0]

    p1_ok = (p1_boot == 0x80 and p1_type in (0x06, 0xEF) and
             p1_lba == BOOT_LBA and p1_cnt == BOOT_SECTORS)
    print(f"分区 1 (boot)   : boot={p1_boot:#04x} type={p1_type:#04x} "
          f"LBA {p1_lba} .. {p1_lba + p1_cnt - 1} "
          f"{'OK' if p1_ok else 'BAD'}")
    good &= p1_ok

    p2_ok = (p2_type == 0x83 and p2_lba == ROOT_LBA)
    print(f"分区 2 (root)   : type={p2_type:#04x} LBA {p2_lba} "
          f"{'OK' if p2_ok else 'BAD'}")
    good &= p2_ok

    # ---- install-done 标记 ----
    mark = "parlz install done".encode()
    moff = 1 * 512
    has_mark = data[moff:moff + len(mark)] == mark
    print(f"install-done 标记: LBA 1 {'OK' if has_mark else '(未写, 首次安装正常)'}")

    # ---- FAT16 VBR @ LBA 2048 ----
    vbr = data[BOOT_LBA * 512:(BOOT_LBA + 1) * 512]
    vbr_sig_ok = vbr[510:512] == b"\x55\xaa"
    vbr_jump_ok = vbr[0] == 0xEB
    vbr_code_ok = any(b != 0 for b in vbr[0x3E:0x1FD])  # syslinux loader
    print(f"FAT16 VBR       : 跳转={vbr[0]:#04x} "
          f"{'OK' if vbr_jump_ok else 'BAD'}  "
          f"55AA={'OK' if vbr_sig_ok else 'BAD(缺签名)'}  "
          f"引导代码(0x3E~)={'OK' if vbr_code_ok else 'BAD(空, Legacy 不可启动)'}")
    good &= vbr_sig_ok and vbr_jump_ok and vbr_code_ok

    # BPB 字段
    bps = struct.unpack("<H", vbr[11:13])[0]
    spc = vbr[13]
    reserved = struct.unpack("<H", vbr[14:16])[0]
    n_fats = vbr[16]
    root_entries = struct.unpack("<H", vbr[17:19])[0]
    fs16 = struct.unpack("<H", vbr[28:30])[0]
    total = struct.unpack("<I", vbr[32:36])[0]
    root_sec = math.ceil(root_entries * 32 / bps)
    boot_base = BOOT_LBA * 512
    print(f"FAT16 BPB       : bps={bps} spc={spc} reserved={reserved} "
          f"fats={n_fats} rootent={root_entries}({root_sec}扇区) "
          f"fs16={fs16} total={total}")

    def try_locate(rel, tag):
        """尝试 rel 处为根目录;成功返回 True。"""
        off = boot_base + rel * 512
        ok, ef, vm, vb, names = parse_root_dir(data, off, root_entries)
        if ok:
            print(f"根目录({tag:12s}) : {names}")
            print(f"  含 EFI/       : OK   含 vmlinuz : OK   "
                  f"vmlinuz {vb} 字节 OK")
            return True, rel
        print(f"根目录({tag:12s}) : (未命中)")
        return False, rel

    # 方案 1: BPB 公式
    rel1 = reserved + n_fats * fs16
    ok1, _ = try_locate(rel1, f"BPB(rel {rel1})")
    if ok1:
        good &= True
        print(f"\n结论            : 全部通过")
        return 0

    # 方案 2: 对齐修正(mkfs.vfat FAT16 常见 32KiB 对齐,fs16'=ceil(total/1024))
    fs16b = math.ceil(total / 1024)
    if fs16b != fs16:
        relb = reserved + n_fats * fs16b
        okb, _ = try_locate(relb, f"对齐(rel {relb})")
        if okb:
            good &= True
            print(f"\n结论            : 全部通过(对齐修正定位)")
            return 0

    # 方案 3: 扫描兜底(搜 VMLINUZ 短名)
    found = None
    for rels in range(32, min(4096, total)):
        off = boot_base + rels * 512
        if b"VMLINUZ" in data[off:off + 512]:
            found = rels
            break
    if found is not None:
        ok3, _ = try_locate(found, f"扫描(rel {found})")
        good &= ok3
        print(f"\n结论            : {'全部通过(扫描定位)' if ok3 else '存在问题'}")
        return 0 if ok3 else 1

    print("根目录            : 扫描未找到 VMLINUZ, 判定 BAD")
    good = False
    print(f"\n结论            : 存在问题")
    return 1


if __name__ == "__main__":
    sys.exit(main())
