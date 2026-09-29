#!/bin/sh
# 固定上游源码在仓库缓存，编译只使用 WSL 本地盘。
set -eu
REPO=/mnt/f/Linux/Parlz
CACHE="$REPO/third_party/distfiles"
WORK=/home/jgzyes/parlz-bash
VERSION=5.3
ARCHIVE="bash-$VERSION.tar.gz"
mkdir -p "$CACHE" "$WORK"
if [ ! -f "$CACHE/$ARCHIVE" ]; then
    curl --fail --location --retry 2 "https://ftp.gnu.org/gnu/bash/$ARCHIVE" -o "$CACHE/$ARCHIVE.part"
    mv "$CACHE/$ARCHIVE.part" "$CACHE/$ARCHIVE"
fi
if [ -f "$CACHE/$ARCHIVE.sha256" ]; then
    (cd "$CACHE" && sha256sum -c "$ARCHIVE.sha256")
else
    (cd "$CACHE" && sha256sum "$ARCHIVE" > "$ARCHIVE.sha256")
fi
if [ ! -f "$WORK/bash-$VERSION/configure" ]; then
    tar -xzf "$CACHE/$ARCHIVE" -C "$WORK"
fi
mkdir -p "$WORK/build"
cd "$WORK/build"
if [ ! -f Makefile ]; then
    CFLAGS='-O2 -fno-pie' LDFLAGS='-static -no-pie' \
      "$WORK/bash-$VERSION/configure" --prefix=/usr --bindir=/bin \
      --enable-static-link --disable-nls --without-bash-malloc \
      --disable-progcomp --disable-loadable-builtins > configure.log 2>&1
fi
make -j"$(nproc)" > build.log 2>&1
if readelf -l bash | grep -q INTERP; then
    printf '%s\n' '错误: Bash 不是静态链接' >&2
    exit 1
fi
./bash --version
python3 "$REPO/scripts/test-bash-features.py" "$WORK/build/bash"
OUT=${1:-/home/jgzyes/parlz-userland/build/bin}
mkdir -p "$OUT"
cp bash "$OUT/bash"
mkdir -p "$REPO/third_party/licenses/bash"
cp "$WORK/bash-$VERSION/COPYING" "$REPO/third_party/licenses/bash/COPYING"
printf '%s\n' 'GNU Bash 5.3 静态构建与 11 项特性验证完成'
