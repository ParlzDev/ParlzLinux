#!/usr/bin/env python3
"""extract-vmlinuz.py - 从 Parlz 安装后的磁盘镜像里提取 vmlinuz。

安装器把内核放在分区 1 的 ext2 文件系统里(/mnt/boot/vmlinuz)。本脚本
解析 ext2 的超级块 -> 组描述符 -> 根 inode -> boot 目录 -> vmlinuz inode,
把文件内容 dd 出来,用于验证"磁盘上的内核确实可引导"。

用法: python3 extract-vmlinuz.py <disk.img> <out-file>
"""
import struct
import sys

PART_OFF = 1 << 20
SB_OFF = PART_OFF + 1024


class Ext2:
    def __init__(self, data):
        self.d = data
        self.block_size = 1024 << struct.unpack_from("<I", data, SB_OFF + 24)[0]
        self.inode_size = struct.unpack_from("<H", data, SB_OFF + 88)[0] or 128
        self.inodes_per_group = struct.unpack_from("<I", data, SB_OFF + 40)[0]
        self.first_ino = struct.unpack_from("<I", data, SB_OFF + 84)[0]
        self.gd_block = 1 if self.block_size >= 4096 else 2

    def blk(self, n):
        off = PART_OFF + n * self.block_size
        return self.d[off:off + self.block_size]

    def group_desc(self, g):
        off = PART_OFF + self.gd_block * self.block_size + g * 32
        return self.d[off:off + 32]

    def inode(self, n):
        """读取 inode n(1-based)。"""
        g = (n - 1) // self.inodes_per_group
        idx = (n - 1) % self.inodes_per_group
        gd = self.group_desc(g)
        itable = struct.unpack_from("<I", gd, 8)[0]
        off = PART_OFF + itable * self.block_size + idx * self.inode_size
        return self.d[off:off + self.inode_size]

    def read_file(self, inode_no):
        raw = self.inode(inode_no)
        size = struct.unpack_from("<I", raw, 4)[0]
        ptrs = list(struct.unpack_from("<15I", raw, 40))
        out = b""
        for p in ptrs[:12]:
            if not p:
                continue
            out += self.blk(p)
        if ptrs[12]:    # 一次间接
            ind = self.blk(ptrs[12])
            n = self.block_size // 4
            for i in range(n):
                p = struct.unpack_from("<I", ind, i * 4)[0]
                if p:
                    out += self.blk(p)
        return out[:size]

    def lookup(self, dir_inode, name):
        """在目录 inode 里查名字,返回子 inode 号(0 表示没有)。"""
        content = self.read_file(dir_inode)
        pos = 0
        while pos + 8 <= len(content):
            ino, rec_len, name_len = struct.unpack_from("<IHB", content, pos)
            if rec_len == 0:
                break
            nm = content[pos + 8:pos + 8 + name_len].decode("latin1")
            if ino and nm == name:
                return ino
            pos += rec_len
        return 0


def main():
    if len(sys.argv) < 3:
        print("usage: extract-vmlinuz.py <disk.img> <out>", file=sys.stderr)
        return 1
    data = open(sys.argv[1], "rb").read()
    fs = Ext2(data)

    root = 2
    boot_ino = fs.lookup(root, "boot")
    if not boot_ino:
        print("磁盘上找不到 /boot", file=sys.stderr)
        return 1
    print(f"/boot inode = {boot_ino}")

    vml_ino = fs.lookup(boot_ino, "vmlinuz")
    if not vml_ino:
        print("磁盘上找不到 /boot/vmlinuz", file=sys.stderr)
        return 1
    print(f"/boot/vmlinuz inode = {vml_ino}")

    blob = fs.read_file(vml_ino)
    # bzImage 不是 ELF:前 512 字节是实模式 setup(bzImage 头),随后才是压缩内核。
    # 有效的 x86 bzImage 在偏移 0x1FE 处有 0x55AA 引导签名。
    if blob[0x1FE:0x200] != b"\x55\xaa":
        print(f"警告: 不像 bzImage (0x1FE 处为 {blob[0x1FE:0x200].hex()})",
              file=sys.stderr)
    else:
        print("校验: bzImage 引导签名 0x55AA 存在")
    open(sys.argv[2], "wb").write(blob)
    print(f"已提取 {len(blob)} 字节 -> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
