import struct
BS=4096
buf=open("/home/jgzyes/dbg.img","rb").read()
# inode bitmap = block 3, block bitmap = block 2
ino_bmp=buf[3*BS:4*BS]
blk_bmp=buf[2*BS:3*BS]
print("ino_bmp (blk3): first 8 bytes =", ino_bmp[:8].hex())
print("  bits set (low 64):", bin(struct.unpack("<Q",ino_bmp[:8])[0])[:40])
print("blk_bmp (blk2): first 8 bytes =", blk_bmp[:8].hex())
# count ino bits set
tot=0
for i in range(BS//8):
    tot+=bin(struct.unpack("<Q",ino_bmp[i*8:i*8+8])[0]).count("1")
print("ino_bmp total bits set = %d (expect 11 + 305 = 316)"%tot)
