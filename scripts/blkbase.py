import struct
BS=4096
buf=open("/home/jgzyes/baseline.img","rb").read()
ino_bmp=buf[3*BS:4*BS]
blk_bmp=buf[2*BS:3*BS]
print("BASELINE ino_bmp first 8:", ino_bmp[:8].hex())
print("BASELINE blk_bmp first 8:", blk_bmp[:8].hex())
# how many bits set
tot=sum(bin(x).count("1") for x in struct.unpack("<32768Q",ino_bmp))
print("BASELINE ino_bmp bits set =",tot)
