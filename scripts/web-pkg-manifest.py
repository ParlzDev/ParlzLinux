#!/usr/bin/env python3
"""从 .pm（未压缩的 newc cpio）流式导出成员清单，给官网演示终端用。

    python3 web-pkg-manifest.py <包.pm> <输出.list>

输出每行：<成员名>\t<mode 八进制>\t<字节数>\t<软链目标，仅软链有>

为什么不下载实体：官网演示里 `pm install core` 是**真下载真解包**（17.9 MiB 可接受），
而 gcc.pm(454.7 MiB) / clang.pm(966.8 MiB) 浏览器里不现实 —— 于是演示端只取这份
清单把成员登记进 VFS（路径/权限/大小/软链目标都是真机的），命令行为另行实现。
"""
import sys

MAGIC = b"070701"
HDR = 110


def align4(n):
    return (n + 3) // 4 * 4


def members(path):
    with open(path, "rb") as f:
        off = 0
        while True:
            f.seek(off)
            hdr = f.read(HDR)
            if len(hdr) < HDR or hdr[:6] != MAGIC:
                return
            def h(i):
                try:
                    return int(hdr[i:i + 8], 16)
                except ValueError:
                    return 0
            mode, size, ns = h(14), h(54), h(94)
            if ns == 0 or ns > 512:
                return
            name = f.read(ns).split(b"\0")[0].decode("utf-8", "replace")
            data = align4(off + HDR + ns)
            if name.startswith("TRAILER"):
                return
            target = ""
            if (mode & 0o170000) == 0o120000 and size:
                f.seek(data)
                target = f.read(size).decode("utf-8", "replace")
            yield name, mode, size, target
            off = align4(data + size)


def main():
    src, out = sys.argv[1], sys.argv[2]
    n = files = links = dirs = 0
    data = 0
    with open(out, "w", encoding="utf-8") as o:
        o.write("# %s: name<TAB>mode<TAB>size<TAB>linktarget\n" % src.split("/")[-1])
        for name, mode, size, target in members(src):
            o.write("%s\t%o\t%d\t%s\n" % (name, mode, size, target))
            n += 1
            kind = mode & 0o170000
            if kind == 0o40000:
                dirs += 1
            elif kind == 0o120000:
                links += 1
            else:
                files += 1
                data += size
    print("%s -> %s: %d 成员 (%d 目录 / %d 文件 / %d 软链), 数据 %d 字节"
          % (src, out, n, dirs, files, links, data))


if __name__ == "__main__":
    main()
