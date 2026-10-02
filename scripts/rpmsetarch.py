#!/usr/bin/env python3
# rpmsetarch.py - 把真 .rpm 的 ARCH 字段原地改成别的架构(造"架构不符"fixture)。
# 用法: python3 scripts/rpmsetarch.py <in.rpm> <out.rpm> <新架构>
#
# 为什么不用 rpmbuild --target i686: rpm 4.18 直接 "No compatible architectures
# found for build" —— 宿主只肯给本机架构出包。而 header 里字符串是
# NUL 结尾、改短不动长度就能原地替换(后面那位本来就是 NUL),
# 所以产物仍是 rpmbuild 写的真包, 只有 ARCH 这一处不同。
import struct
import sys


def main(path, out_path, newarch):
    d = bytearray(open(path, "rb").read())
    if d[:4] != b"\xed\xab\xee\xdb":
        raise SystemExit("lead 魔数不对")

    def section(off):
        if d[off:off + 3] != b"\x8e\xad\xe8":
            raise SystemExit("offset %d 不是 header" % off)
        nidx = struct.unpack(">i", bytes(d[off + 8:off + 12]))[0]
        dlen = struct.unpack(">i", bytes(d[off + 12:off + 16]))[0]
        store = off + 16 + nidx * 16
        return store, store + dlen

    off = 96
    store, end = section(off)                    # 签名 header
    off = end + ((8 - end % 8) % 8)
    store, end = section(off)                    # 主 header(off 就是它的起点)
    nidx = struct.unpack(">i", bytes(d[off + 8:off + 12]))[0]
    nb = len(newarch.encode()) + 1
    for i in range(nidx):
        p = off + 16 + i * 16
        tag, typ, dat, cnt = struct.unpack(">iiii", bytes(d[p:p + 16]))
        if tag != 1022 or typ != 6:               # 1022 = ARCH, 6 = 字符串
            continue
        o = store + dat
        old = bytes(d[o:d.index(0, o)])
        if nb > len(old) + 1:
            raise SystemExit("新架构名 %s 比原 %s 长, 原地替换不了" %
                             (newarch, old.decode()))
        d[o:o + nb] = newarch.encode() + b"\0"
        open(out_path, "wb").write(bytes(d))
        print("%s: ARCH %s -> %s" % (path, old.decode(), newarch))
        return
    raise SystemExit("header 里没有 ARCH(1022) 标签")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit("用法: rpmsetarch.py <in.rpm> <out.rpm> <新架构>")
    main(sys.argv[1], sys.argv[2], sys.argv[3])
