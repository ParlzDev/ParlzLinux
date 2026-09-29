#!/usr/bin/env python3
# 插桩: 从 gen-fatboot.sh 提取 heredoc Python, 在每次 data 写操作前后记录 len(data),
# 定位哪个 slice 赋值把固定尺寸 bytearray 扩位。
import re

src = open("/mnt/f/Linux/Parlz/scripts/gen-fatboot.sh", encoding="utf-8", errors="replace").read()
m = re.search(r"python3 - \"\$IMG\" \"\$OUT\".*?<<'PYEOF'\n(.*?)\nPYEOF", src, re.S)
assert m, "找不到主 heredoc"
code = m.group(1)

# argv 解析行 → 硬编码(与 shell 传参一致)
argv_assignment = re.search(
    r"IMG, OUT, VML, SYSLINUX_DIR, EFI64_DIR, EFI64_EFI, TOTAL_S, SPC, VBR_CODE_F = \\\n"
    r"    sys\.argv\[1\], sys\.argv\[2\], sys\.argv\[3\], sys\.argv\[4\], \\\n"
    r"    sys\.argv\[5\], sys\.argv\[6\], int\(sys\.argv\[7\]\), int\(sys\.argv\[8\]\), sys\.argv\[9\]",
    code)
if argv_assignment:
    code = code[:argv_assignment.start()] + (
        'IMG, OUT, VML, SYSLINUX_DIR, EFI64_DIR, EFI64_EFI, TOTAL_S, SPC, VBR_CODE_F = \\\n'
        '    "/home/jgzyes/parlz-fatboot.img", "/mnt/f/Linux/Parlz/userland/bootfat.h", \\\n'
        '    "/home/jgzyes/parlz-userland/root/boot/vmlinuz", "/usr/lib/syslinux", \\\n'
        '    "/usr/lib/syslinux/modules/efi64", "/usr/lib/SYSLINUX.EFI/efi64/syslinux.efi", \\\n'
        '    int("81920"), int("2"), "/home/jgzyes/vbr-code.bin"'
    ) + code[argv_assignment.end():]

# 屏蔽最终写盘与 C 数组输出(不影响 len(data) 追踪)
code = code.replace("open(IMG, \"wb\").write(bytes(data))", "pass  # probe: skip write")
code = code.replace('assert len(data) == TOTAL_S * 512', 'pass  # probe: assert off')
code = code.replace("img = open(IMG, \"rb\").read()", "img = b\"\"  # probe")
code = code.replace("with open(OUT, \"w\") as o:", "if False:  # probe")

# 在 4 个 data 写操作前插探针行
anchors = [
    ("VBR写", "data[0:512] = vbr"),
    ("FAT写", "data[off:off + len(fatbytes)] = fatbytes"),
    ("根目录写", "data[rd_off:rd_off + len(root_dir_data)] = root_dir_data"),
    ("数据循环", "data[off:off + 512] = chunk"),
]
for name, anchor in anchors:
    idx = code.find(anchor)
    assert idx > 0, "锚点缺失: %s" % anchor
    ln = code.rfind("\n", 0, idx) + 1
    indent = " " * (idx - ln)
    probe = '%sassert len(data) == TOTAL_S * 512 or print("探针[%s] 前 len(data)=%d, 已扩位")\n' % (
        indent, name, len(data))
    code = code[:ln] + probe + code[ln:]

ns = {"__name__": "__main__"}
exec(compile(code, "<probe>", "exec"), ns)
print("OK: 无扩位")
