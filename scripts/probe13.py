import struct
BS = 4096
img = "/home/jgzyes/final.img"
buf = open(img, "rb").read()

# --- inode table @ block 4 ---
tab = 4 * BS
i13 = buf[tab + 12 * 128:tab + 13 * 128]
ib = list(struct.unpack("<15I", i13[40:100]))
print("ino13 on disk: size=%d ib[0]=%d ib[12]=%d ib[13]=%d" % (
    struct.unpack("<I", i13[4:8])[0], ib[0], ib[12], ib[13]))

l2b = ib[13]
l2 = buf[l2b * BS:(l2b + 1) * BS]
l2slots = [p for p in struct.unpack("<1024I", l2) if p]
print("L2 block %d slots: %s" % (l2b, l2slots))
for k, p in enumerate(l2slots):
    l1 = buf[p * BS:(p + 1) * BS]
    ptrs = [q for q in struct.unpack("<1024I", l1) if q]
    print("  L1 blk %d: %d ptrs, first=%s last=%s" % (
        p, len(ptrs), ptrs[:2], ptrs[-2:] if len(ptrs) > 2 else []))

# data block content check: first data block of L1[0]
if l2slots:
    firstdata = struct.unpack("<1024I", buf[l2slots[0] * BS:l2slots[0] * BS + BS])[0]
    if firstdata:
        d = buf[firstdata * BS:firstdata * BS + 16]
        ref = open("/home/jgzyes/testroot/bin/big.bin", "rb").read()
        print("first data block %d content matches source: %s" % (
            firstdata, d == ref[:16]))

# root dir entries
i2 = buf[tab:tab + 128]
ib2 = list(struct.unpack("<15I", i2[40:100]))
db = buf[ib2[0] * BS:ib2[0] * BS + BS]
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
print("root entries: %s" % ents)
