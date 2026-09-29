#!/bin/sh
# OpenSSL LTS 源码与构建缓存保留在 WSL 本地盘，产物只使用静态库。
set -eu
export PATH=/usr/bin:/bin
REPO=/mnt/f/Linux/Parlz
VERSION=3.5.8
WORK=/home/jgzyes/parlz-openssl
PREFIX="$WORK/stage"
DIST="$REPO/third_party/distfiles"
BASE="https://github.com/openssl/openssl/releases/download/openssl-$VERSION"
mkdir -p "$DIST" "$WORK"
cd "$DIST"
for suffix in tar.gz tar.gz.sha256; do
    file="openssl-$VERSION.$suffix"
    if [ ! -s "$file" ]; then
        curl -fL --retry 2 --connect-timeout 20 --max-time 300 "$BASE/$file" -o "$file.part"
        mv "$file.part" "$file"
    fi
done
python3 - "$VERSION" <<'PY'
import hashlib, pathlib, sys
name = 'openssl-' + sys.argv[1] + '.tar.gz'
expected = pathlib.Path(name + '.sha256').read_text().split()[0]
actual = hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest()
if actual != expected:
    raise SystemExit('OpenSSL SHA256 校验失败')
print('OpenSSL SHA256:', actual)
PY
if [ ! -d "$WORK/openssl-$VERSION" ]; then
    tar -xzf "$DIST/openssl-$VERSION.tar.gz" -C "$WORK"
fi
cd "$WORK/openssl-$VERSION"
if [ ! -f "$PREFIX/.parlz-$VERSION" ]; then
    ./Configure linux-x86_64 no-shared no-module no-dso no-tests \
        --prefix="$PREFIX" --openssldir=/etc/ssl --libdir=lib \
        -O2 -fno-pie -static -no-pie > "$WORK/configure.log" 2>&1
    make -j"$(nproc)" > "$WORK/build.log" 2>&1
    make install_sw > "$WORK/install.log" 2>&1
    if readelf -l "$PREFIX/bin/openssl" | grep -q INTERP; then
        echo '错误：OpenSSL 仍依赖动态解释器'; exit 1
    fi
    touch "$PREFIX/.parlz-$VERSION"
fi
mkdir -p "$REPO/third_party/licenses/openssl"
cp LICENSE.txt "$REPO/third_party/licenses/openssl/"
"$PREFIX/bin/openssl" version -a
