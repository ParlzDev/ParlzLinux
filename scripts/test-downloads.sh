#!/bin/sh
# 独立构建下载器，不碰正在构建的用户空间和内核。仅运行本地测试。
# TLS 后端:OpenSSL 静态库(scripts/build-openssl.sh 的产物)。
set -eu
export PATH=/usr/bin:/bin
SRC=/mnt/f/Linux/Parlz
WORK=${DOWNLOAD_TEST_WORK:-/home/jgzyes/parlz-download-tests}
OSSL=/home/jgzyes/parlz-openssl/stage
mkdir -p "$WORK/bin"
[ -f "$OSSL/lib/libssl.a" ] || { echo "缺 $OSSL/lib/libssl.a,先跑 scripts/build-openssl.sh"; exit 1; }
for tool in curl wget; do
    gcc -O2 -g -Wall -Wextra -Werror -std=c17 -D_GNU_SOURCE -fno-pie -no-pie -static \
        -DPARLZ_HAVE_OPENSSL=1 -I "$OSSL/include" \
        "$SRC/userland/$tool.c" "$SRC/userland/http_client.c" \
        "$OSSL/lib/libssl.a" "$OSSL/lib/libcrypto.a" -lm -lpthread \
        -o "$WORK/bin/$tool"
done
# 同时验证无 TLS 构建也不会静默降级到 HTTP。
gcc -O2 -Wall -Wextra -Werror -std=c17 -D_GNU_SOURCE -fno-pie -no-pie -static \
    "$SRC/userland/curl.c" "$SRC/userland/http_client.c" -o "$WORK/bin/curl-no-tls"
python3 "$SRC/scripts/test-downloads.py" "$WORK" > "$WORK/results.log" 2>&1 || {
    python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).read_text())' "$WORK/results.log"
    exit 1
}
python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).read_text())' "$WORK/results.log"
