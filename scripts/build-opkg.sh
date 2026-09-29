#!/bin/sh
# 真正的上游 OPKG，编译仅在 WSL 本地盘进行，不安装到宿主机。
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD=${OPKG_BUILD_DIR:-/home/jgzyes/parlz-opkg}
case "$BUILD" in /mnt/*|/|'') printf '%s\n' '请使用 WSL 本地盘的独立构建目录' >&2; exit 1;; esac
VENDOR="$REPO/vendor"
printf '%s  %s\n' 582a4c9e220cce0b7d08a0915c95db9f24ae43ad372ed5d42d5ec5f6cd413f0c "$VENDOR/opkg-0.8.0.tar.gz" | sha256sum -c -
# opkg 上游硬依赖 libarchive(静态链接):宿主需 libarchive-dev 静态库
# (24.04: apt-get install -y libarchive-dev pkg-config)。缺失时 configure
# 直接 abort, 这里先探测给出可操作提示。
if [ ! -e /usr/lib/x86_64-linux-gnu/libarchive.a ]; then
    printf '错误：缺 libarchive 静态库(宿主 WSL)，先跑：\n' >&2
    printf '  apt-get install -y libarchive-dev pkg-config\n' >&2
    exit 1
fi
mkdir -p "$BUILD" "$BUILD/bin"
# 仅覆盖此脚本自己的解包目录，不触碰用户空间全量构建目录。
if [ ! -d "$BUILD/upstream" ]; then
    mkdir "$BUILD/upstream"
    tar -xzf "$VENDOR/opkg-0.8.0.tar.gz" --strip-components=1 -C "$BUILD/upstream"
fi
cd "$BUILD/upstream"
./configure --prefix=/usr --bindir=/bin --sysconfdir=/etc --localstatedir=/var \
    --disable-shared --enable-static --with-static-libopkg \
    --disable-curl --disable-ssl-curl --disable-gpg --enable-sha256 \
    --enable-xz --enable-bzip2 --enable-lz4 --enable-zstd \
    --with-libsolv=no \
    CFLAGS='-O2 -fno-pie' LDFLAGS='-static -no-pie' > "$BUILD/configure.log" 2>&1
# configure 不能接受 libtool 的 -all-static；仅在 make 阶段使用。
make -j"${JOBS:-4}" LDFLAGS='-all-static -no-pie' > "$BUILD/build.log" 2>&1
install -m 755 src/opkg "$BUILD/bin/opkg-native"
cc -O2 -Wall -Wextra -Werror -fno-pie -no-pie -static \
    "$REPO/userland/ppm.c" -o "$BUILD/bin/ppm"
cc -O2 -Wall -Wextra -Werror -fno-pie -no-pie -static \
    "$REPO/userland/opkg.c" -o "$BUILD/bin/opkg"
for binary in opkg-native ppm opkg; do
    if readelf -l "$BUILD/bin/$binary" | grep -q INTERP; then
        printf '错误：%s 并非静态程序\n' "$binary" >&2; exit 1
    fi
    file "$BUILD/bin/$binary"
done
"$BUILD/bin/opkg-native" --version
# 集成用完整暂存树，可合并至新 rootfs（不要覆盖现有机器的配置）。
STAGE="$BUILD/stage"
mkdir -p "$STAGE/bin" "$STAGE/etc/opkg" "$STAGE/var/lib/opkg" \
    "$STAGE/usr/share/opkg/intercept" "$STAGE/usr/share/licenses/opkg"
install -m 755 "$BUILD/bin/opkg-native" "$BUILD/bin/opkg" "$BUILD/bin/ppm" "$STAGE/bin/"
install -m 755 intercept/ldconfig intercept/depmod intercept/update-modules "$STAGE/usr/share/opkg/intercept/"
install -m 644 COPYING "$STAGE/usr/share/licenses/opkg/COPYING"
printf '%s\n' '# Parlz OPKG：仅配置可信且 ABI 兼容的源，不默认订阅第三方系统源。' \
    'dest root /' 'arch all 1' 'arch noarch 1' 'arch x86_64 10' \
    'option lists_dir /var/lib/opkg/lists' > "$STAGE/etc/opkg/opkg.conf"
printf '编译完成：%s/bin，集成暂存树：%s（未修改宿主机或 initramfs）\n' "$BUILD" "$STAGE"
