#!/usr/bin/env python3
"""newc cpio 归档逆向布局探针: 在归档里逐字节定位 070701 头, 按候选
字段偏移解析 fsize/namesize, 验证下一头指针, 全成员走位命中 + 总数
与 cpio -t 基准比对。多组偏移假设穷举, 命中即输出该组。
跑: wsl -d Ubuntu-26.04 -e python3 /mnt/f/Linux/Parlz/scripts/probe-cpio-layout.py <pkg> <cpio_t_total>
"""
import sys

data = open(sys.argv[1], "rb").read()
N = len(data)
BASELINE = int(sys.argv[2])

def rd(h, o):
    s = h[o:o+8]
    try:
        return int(s.decode("ascii"), 16)
    except Exception:
        return None

# 按 cpio -t 基准, 找 070701 所有位置(按 110 步进网格 + 自由网格)
mag = []
i = 0
while True:
    j = data.find(b"070701", i)
    if j < 0:
        break
    mag.append(j)
    i = j + 1
print("070701 共出现 %d 处; 基准 cpio -t = %d" % (len(mag), BASELINE))

# 候选字段偏移组 (fsize_off, namesize_off): 假设头=110, name 紧跟 +110,
# 数据 4 对齐; 走位: off+=110; off+=namesize; off=(off+3)&~3; off+=(fsize+3)&~3
# 试 (54,94) 与 (64,96) 等
combos = [(54, 94), (54, 96), (64, 96), (64, 94), (46, 86), (46, 88)]
for fs_off, ns_off in combos:
    off = 0
    n = 0
    bad = 0
    max_off = -1
    seq_ok = True
    while off + 110 <= N:
        if data[off:off+6] != b"070701":
            # 头不在 off: 找下一个 070701
            j = data.find(b"070701", off)
            if j < 0 or j + 110 > N:
                break
            seq_ok = False
            off = j
            continue
        fsize = rd(data, off + fs_off)
        namesize = rd(data, off + ns_off)
        if fsize is None or namesize is None:
            bad += 1
            break
        off2 = off + 110 + namesize
        off2 = (off2 + 3) & ~3
        off2 += (fsize + 3) & ~3
        if off2 > N:
            bad += 1
            break
        max_off = off2
        off = off2
        n += 1
        if n > 100000:
            break
    verdict = "OK" if (n >= BASELINE - 3 and n <= BASELINE + 2 and not bad) else "x"
    print("  fsize@%d namesize@%d: 走 %d 成员, seq_ok=%s, 坏=%d -> %s"
          % (fs_off, ns_off, n, seq_ok, bad, verdict))
