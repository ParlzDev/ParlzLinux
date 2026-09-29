#!/bin/sh
# mkfs-test.sh - 在 WSL 里对 ext2 disk image 做诊断:
#   1. 读我们 mkfs 写的超块,看 s_magic/s_blocks_count/s_inodes_count 等
#   2. 用宿主机的 dumpe2fs (若有) 验证文件系统是否合法
#   3. 若合法,说明内核 mount 失败是分区/设备问题而非 fs 本身
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/mkfs-test.sh <disk.img>
set -e
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=$1
[ -z "$IMG" ] && { echo "usage: $0 <disk.img>"; exit 1; }
[ -f "$IMG" ] || { echo "no such file $IMG"; exit 1; }

echo "=== 1. 读取分区 1 (LBA 2048) 的 ext2 超块 ==="
# 分区 1 在磁盘偏移 1MiB;ext2 超块在分区内 4096 字节(块 1)
SB_OFF=$(( 1048576 + 4096 ))
python3 - "$IMG" "$SB_OFF" <<'PY'
import sys
data = open(sys.argv[1],'rb').read()
off = int(sys.argv[2])
import struct
vals = {}
for name, fo in [
    ('s_inodes_count',0),('s_blocks_count',4),('s_r_blocks_count',8),
    ('s_free_blocks_count',12),('s_free_inodes_count',16),
    ('s_first_data_block',20),('s_log_block_size',24),
    ('s_log_frag_size',28),('s_blocks_per_group',32),
    ('s_inodes_per_group',36),('s_mtime',40),('s_wtime',44),
    ('s_mnt_count',48),('s_max_mnt_count',50),('s_magic',52),
    ('s_state',54),('s_errors',56),('s_minor_rev_level',58),
    ('s_lastcheck',60),('s_checkinterval',64),('s_creator_os',68),
    ('s_rev_level',72),('s_first_ino',192),('s_inode_size',196),
]:
    vals[name] = struct.unpack_from('<I', data, off+fo)[0] if fo<192 else struct.unpack_from('<H', data, off+fo)[0]
for k,v in vals.items():
    print(f"  {k} = {v}")
PY

echo
echo "=== 2. dumpe2fs 诊断(若有) ==="
if command -v dumpe2fs >/dev/null 2>&1; then
    dumpe2fs -h <(dd if="$IMG" bs=1M skip=1 count=64) 2>&1 | head -25 || \
    echo "  dumpe2fs 失败"
else
    echo "  无 dumpe2fs,跳过"
fi

echo
echo "=== 3. 直接 mount 诊断(只读) ==="
PART=/tmp/parlz-part-test.img
dd if="$IMG" bs=1M skip=1 of="$PART" 2>/dev/null
rm -rf /tmp/parlz-mnt; mkdir -p /tmp/parlz-mnt
if mount -o ro,loop "$PART" /tmp/parlz-mnt 2>&1; then
    echo "  MOUNT OK!"; ls /tmp/parlz-mnt; umount /tmp/parlz-mnt
else
    echo "  mount 失败(分区文件系统有问题)"
fi
