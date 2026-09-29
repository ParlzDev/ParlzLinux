import struct
BS = 4096
buf = open("/home/jgzyes/dbg.img", "rb").read()

# 1) GDT on-disk
gdt = struct.unpack("<III", buf[BS:BS + 12])
print("GDT on-disk: bbm=%d ibm=%d itb=%d" % gdt)

# 2) locate the real inode table: scan blocks 1..8 for 0x41FF (lost+found)
for b in range(1, 9):
    blk = buf[b * BS:(b + 1) * BS]
    i = blk.find(struct.pack("<H", 0x41FF), 0)
    print("blk%d: 0x41FF at byte %s" % (b, i if i >= 0 else None))

# 3) locate ino12..ino15 (0x41ED / 0x81A4)
for b in range(1, 9):
    blk = buf[b * BS:(b + 1) * BS]
    for pat in (0x41ED, 0x81A4):
        off = 0
        while True:
            i = blk.find(struct.pack("<H", pat), off)
            if i < 0:
                break
            if i % 128 == 0:
                print("blk%d: 0x%04X at inode slot %d" % (b, pat, i // 128 + 1))
            off = i + 2
