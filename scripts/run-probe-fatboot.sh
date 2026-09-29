#!/bin/sh
# 最小复现: 完整跑 gen-fatboot.sh 的 data 构建段(提取 heredoc), 逐步打 len
python3 - /mnt/f/Linux/Parlz/scripts/gen-fatboot.sh <<'PYEOF'
import sys, re
src = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(r"python3 - \"\$IMG\" \"\$OUT\".*?<<'PYEOF'\n(.*?)\nPYEOF", src, re.S)
assert m, "heredoc not found"
code = m.group(1)

argv_assignment = re.search(
    r"IMG, OUT, VML, SYSLINUX_DIR, EFI64_DIR, EFI64_EFI, TOTAL_S, SPC, VBR_CODE_F = \\\n"
    r"    sys\.argv\[1\], sys\.argv\[2\], sys\.argv\[3\], sys\.argv\[4\], \\\n"
    r"    sys\.argv\[5\], sys\.argv\[6\], int\(sys\.argv\[7\]\), int\(sys\.argv\[8\]\), sys\.argv\[9\]",
    code)
assert argv_assignment, "argv line not found"
code = code[:argv_assignment.start()] + (
    'IMG, OUT, VML, SYSLINUX_DIR, EFI64_DIR, EFI64_EFI, TOTAL_S, SPC, VBR_CODE_F = \\\n'
    '    "/home/jgzyes/parlz-fatboot.img", "/mnt/f/Linux/Parlz/userland/bootfat.h", \\\n'
    '    "/home/jgzyes/parlz-userland/root/boot/vmlinuz", "/usr/lib/syslinux", \\\n'
    '    "/usr/lib/syslinux/modules/efi64", "/usr/lib/SYSLINUX.EFI/efi64/syslinux.efi", \\\n'
    '    int("81920"), int("2"), "/home/jgzyes/vbr-code.bin"'
) + code[argv_assignment.end():]

# 定位 "组装字节" 段: 从 data = bytearray 到 数据写循环结束(open(IMG 之前)
start = code.find("data = bytearray(TOTAL_S * 512)")
end = code.find("open(IMG")
assert start > 0 and end > start, "段定位失败"
seg = code[:start] + code[start:end]
seg = seg.replace("data = bytearray(TOTAL_S * 512)",
                  "data = bytearray(TOTAL_S * 512)\n"
                  "def _chk(tag):\n"
                  "    L = len(data)\n"
                  "    if L != TOTAL_S * 512:\n"
                  "        print('probe: %s → len(data)=%d (期望 %d) 上界: 检查前一行' % (tag, L, TOTAL_S * 512))\n"
                  "_chk('init')")
# 在每个数据写操作后插 _chk
for tag, anchor in [
    ("vbr", "data[0:512] = vbr"),
    ("fat", "data[off:off + len(fatbytes)] = fatbytes"),
    ("rootdir", "data[rd_off:rd_off + len(root_dir_data)] = root_dir_data"),
]:
    seg = seg.replace(anchor, anchor + "\n_chk('%s')" % tag)
# 数据循环内: 替换写 chunk 行 + 其后两行
seg = seg.replace(
    "data[off:off + 512] = chunk\n"
    "        written += 512\n"
    "        off += 512",
    "data[off:off + 512] = chunk\n"
    "        _chk('datalog')\n"
    "        written += 512\n"
    "        off += 512")

seg += "\n_chk('final')\nprint('probe: 段结束 len=%d' % len(data))\n"
ns = {"__name__": "__main__"}
exec(compile(seg, "<seg>", "exec"), ns)
PYEOF
