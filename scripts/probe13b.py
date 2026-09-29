import struct
BS = 4096
buf = open("/home/jgzyes/final.img", "rb").read()
tab = 4 * BS

# ino2 full i_block
i2 = buf[tab:tab + 128]
ib2 = list(struct.unpack("<15I", i2[40:100]))
print("ino2 i_block all 15:", ib2)
print("ino2 size=%d" % struct.unpack("<I", i2[4:8])[0])

# scan block 261 directly
db = buf[261 * BS:262 * BS]
off = 0
ents = []
while off + 8 <= BS:
    ino_no = struct.unpack("<I", db[off:off + 4])[0]
    if ino_no == 0:
        break
    rl = struct.unpack("<H", db[off + 4:off + 6])[0]
    if rl == 0:
        break
    ents.append(db[off + 8:off + 8 + db[off + 6]].decode("latin1", "replace"))
    off += rl
print("block 261 entries: %s" % ents)

# L1 blk 276 full scan
l1 = buf[276 * BS:277 * BS]
ptrs = [q for q in struct.unpack("<1024I", l1) if q]
print("L1 blk 276 nonzero count=%d" % len(ptrs))
# check block 275 (first L1 slot)
d = buf[275 * BS:275 * BS + 16]
print("block 275 first16:", d.hex())
