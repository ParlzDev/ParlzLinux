#!/bin/sh
rm -f /tmp/real-mbr.img
python3 - <<PY
import re
d=open("/home/jgzyes/parlz-disk.img","rb").read()
real=open("/usr/lib/syslinux/mbr/mbr.bin","rb").read()
print("真实 syslinux mbr.bin 长度:", len(real))
print("真实 mbr.bin 前 16:", real[:16].hex(" "))
print("真实 mbr.bin 末 2:", real[-2:].hex())
d2=bytearray(d)
d2[0:len(real)]=real
open("/tmp/real-mbr.img","wb").write(bytes(d2))
print("已写真实 syslinux mbr.bin -> /tmp/real-mbr.img")
PY
echo "=== SeaBIOS virtio + 真实 syslinux MBR ==="
timeout 45 qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -drive file=/tmp/real-mbr.img,if=virtio,format=raw \
  -boot c -nographic -monitor none -no-reboot \
  </dev/null 2>&1 | grep -aE "Booting|bootable|Parlz|Linux version|error|kernel|vmlinuz|Parlz shell" | head -12
