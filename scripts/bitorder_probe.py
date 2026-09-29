# 两种位序都试: 用 zlib inflate 做对照(标准答案), 看哪种位序解出
import sys, zlib

d = open('/tmp/ctrl_deflate.bin','rb').read()

def read_seq(data, n_bits, msb_first):
    """按 msb_first/msb 两种位序读出前 n_bits 位的整数"""
    out = []
    for i in range(n_bits):
        byte = data[i // 8]
        if msb_first:
            bit = (byte >> (7 - i % 8)) & 1
        else:
            bit = (byte >> (i % 8)) & 1
        out.append(bit)
    return out

# 先读 3 bits (bfinal+btype):
for msb in (False, True):
    seq = read_seq(d, 3, msb)
    print("%s: 前3bit=%s (bfinal=%d btype=%d)" % (
        "MSB-first" if msb else "LSB-first", seq,
        seq[0], (seq[1]<<1)|seq[2] if not msb else seq[1]*2+seq[2]))
