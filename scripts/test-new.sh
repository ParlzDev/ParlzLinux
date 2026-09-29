#!/bin/bash
# 编译新增/修改的 userland 命令并快速测试
set -e
cd /mnt/f/Linux/Parlz/userland
CFLAGS="-static -O2 -Wall -fno-pie -Wno-unused-function"
OUT=/home/jgzyes/parlz-userland/test-out
mkdir -p $OUT

echo "=== tree.c ==="
gcc $CFLAGS -o $OUT/tree tree.c
echo OK tree

echo "=== ping.c ==="
gcc $CFLAGS -o $OUT/ping ping.c
echo OK ping

echo "=== wget.c (no pazesl) ==="
gcc $CFLAGS -o $OUT/wget wget.c
echo OK wget

echo "=== curl.c (no pazesl, 验证括号) ==="
gcc $CFLAGS -o $OUT/curl curl.c
echo OK curl

echo "=== sh.c ==="
gcc $CFLAGS -o $OUT/sh sh.c
echo OK sh

# 快速功能测试
cd /tmp
echo "=== tree 功能测试 ==="
rm -rf /tmp/testtree
mkdir -p /tmp/testtree/a/b/c /tmp/testtree/d
touch /tmp/testtree/f1 /tmp/testtree/a/f2 /tmp/testtree/d/f3
$OUT/tree /tmp/testtree
echo "exit=$?"

echo "=== tree -d 只看目录 ==="
$OUT/tree -d /tmp/testtree

echo "=== ping 本地回环(无 root 会失败,只看输出) ==="
$OUT/ping -c 1 127.0.0.1 || true

echo "=== wget 帮助 ==="
$OUT/wget || true

echo "=== curl 帮助(验证括号匹配) ==="
$OUT/curl || true
