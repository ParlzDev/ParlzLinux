#!/usr/bin/env python3
# rpmhdr.py - 打印 .rpm 主 header 的标签表(写 userland/rpm.c 时的宿主侧探针)。
# 用法: python3 scripts/rpmhdr.py <file.rpm>
# 目的: 标签号与类型不靠记忆 —— 与真 rpm/rpmbuild 的输出现场核对。
import struct
import sys

# header 数据类型(rpm.org 的 rpmHeaderDataType 枚举, 号不能靠记忆):
#   0 NULL, 1 CHAR, 2 INT8, 3 INT16, 4 INT32, 5 INT64,
#   6 STRING, 7 BIN, 8 STRING_ARRAY(count=**字节数**), 9 I18NSTRING(count=串数)
ALIGN = {0: 1, 1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 1, 9: 1}


def section(d, off):
    """返回 (魔数版本, 索引项, 值区起点 store, 下一段起点)。
    头 16 字节实测布局(与 rpm.org 文档一致): magic(4) + reserved(4)
    + nindex(4) + 值区长度(4)。值区紧跟索引之后, 项内 offset 相对值区起点。
    (别把 reserved 当成 nindex 往下读: nindex 会变成 0, 然后整段解析全废。)"""
    if d[off:off + 3] != b"\x8e\xad\xe8":
        raise SystemExit("offset %d 不是 header 魔数 8ead e8: %s" %
                         (off, d[off:off + 4].hex()))
    ver = d[off + 3]
    nidx, dlen = struct.unpack(">ii", d[off + 8:off + 16])
    ents = [struct.unpack(">iiii", d[off + 16 + i * 16:off + 32 + i * 16])
            for i in range(nidx)]
    store = off + 16 + nidx * 16
    end = store + dlen                        # 值区末尾 = 负载起点(不补齐)
    nxt = end
    while nxt % 8:                            # 只有段与段之间才 8 字节对齐
        nxt += 1
    return ver, ents, store, end, nxt


def val(d, ent, store):
    tag, typ, dat, cnt = ent
    a = ALIGN.get(typ, 1)
    o = store + dat
    if a > 1:
        o += (-dat) % a                       # 对齐相对 store 起点算
    if typ == 0:
        return None
    if typ == 1:
        return list(struct.unpack(">%db" % cnt, d[o:o + cnt]))
    if typ == 2:
        return list(struct.unpack(">%dB" % cnt, d[o:o + cnt]))
    if typ == 3:
        return list(struct.unpack(">%dh" % cnt, d[o:o + 2 * cnt]))
    if typ == 4:
        return list(struct.unpack(">%di" % cnt, d[o:o + 4 * cnt]))
    if typ == 5:
        return list(struct.unpack(">%dq" % cnt, d[o:o + 8 * cnt]))
    if typ == 7:
        return d[o:o + cnt]
    if typ == 6:                              # 单个 C 串
        return d[o:d.index(b"\0", o)].decode("utf8", "replace")
    if typ == 8:                              # 串数组: cnt 是**字节数**
        blob = d[o:o + cnt]
        parts = blob.split(b"\0")
        if parts and parts[-1] == b"":
            parts.pop()
        return [p.decode("utf8", "replace") for p in parts]
    if typ == 9:                              # i18n 串数组: cnt 是串数
        out = []
        q = o
        for _ in range(cnt):
            e = d.index(b"\0", q)
            out.append(d[q:e].decode("utf8", "replace"))
            q = e + 1
        return out
    return "?"


def main(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\xed\xab\xee\xdb", "lead 魔数不对: " + d[:4].hex()
    print("lead name: %s" % d[10:76].split(b"\0")[0].decode())
    ver, ents, store, end, nxt = section(d, 96)
    print("signature header: %d 项 (ver=%d)" % (len(ents), ver))
    ver, ents, store, end, nxt = section(d, nxt)
    print("main header: %d 项, store=%d, 负载@%d = %s" %
          (len(ents), store, end, d[end:end + 4].hex()))
    for e in sorted(ents):
        v = val(d, e, store)
        s = str(v)
        if len(s) > 70:
            s = s[:70] + "..."
        print("  tag %-6d type=%d cnt=%-6d %s" % (e[0], e[1], e[3], s))


if __name__ == "__main__":
    main(sys.argv[1])
