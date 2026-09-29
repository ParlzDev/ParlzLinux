#!/usr/bin/env python3
# dec-vga.py - 解析 QEMU monitor 的 `xp /4000xb 0xb8000` 输出为可读文本。
# 用法: python3 dec-vga.py <raw-monitor-output>
# VGA 文本模式: 偶数下标 = 字符, 奇数下标 = 属性; 每行 80 字符。
import re, sys

vals = []
for line in open(sys.argv[1], errors="ignore"):
    line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line)
    m = re.match(r"^[0-9a-f]+:\s+(.*)$", line.strip())
    if not m:
        continue
    for tok in m.group(1).split():
        tok = tok.lower().removeprefix("0x")
        if re.fullmatch(r"[0-9a-f]{2}", tok):
            vals.append(int(tok, 16))

chars = []
for i in range(0, len(vals) - 1, 2):
    c = vals[i]
    chars.append(chr(c) if 32 <= c < 127 else ("\n" if c in (0x0a, 0x0d, 0x0c) else " "))

print("VGA 文本缓冲(%d 字节, %d 字符):" % (len(vals), len(chars)))
for i in range(0, len(chars), 80):
    line = "".join(chars[i:i + 80]).rstrip()
    if line.strip():
        print("  |%-80s|" % line)
