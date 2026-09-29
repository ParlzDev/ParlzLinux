#!/usr/bin/env python3
"""解 DEFLATE dynamic 头的 clens, 打印按码长值 0..18 索引的码长表。"""
import sys

def read_bit0first(data, bitpos, n):
    v = 0
    for i in range(n):
        byte = data[bitpos // 8]
        bit = (byte >> (bitpos % 8)) & 1
        v |= bit << i
        bitpos += 1
    return v, bitpos

d = open('/tmp/ctrl_deflate.bin', 'rb').read()
bp = 0
bfinal, bp = read_bit0first(d, bp, 1)
btype, bp = read_bit0first(d, bp, 2)
hlit, bp = read_bit0first(d, bp, 5)
hlit += 257
hdist, bp = read_bit0first(d, bp, 5)
hdist += 1
hlen, bp = read_bit0first(d, bp, 4)
hlen += 4
print('bfinal=%d btype=%d hlit=%d hdist=%d hlen=%d' %
      (bfinal, btype, hlit, hdist, hlen))
order = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
# 按码长值索引: clens[codevalue]
clens = [0] * 19
for i in range(hlen):
    v, bp = read_bit0first(d, bp, 3)
    clens[order[i]] = v
print('clens[码长值] =', clens)
# 按 slot 顺序(=order 顺序)
by_slot = [0] * len(order)
# 重新读
bp2 = 3 + 5 + 5 + 4 + hlen * 3
for i in range(hlen):
    v, _ = read_bit0first(d, bp2 + i * 3, 3)
    by_slot[i] = v
print('by slot (slot_i -> 码长):', list(zip(order, by_slot)))
