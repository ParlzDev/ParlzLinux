#!/bin/bash
# build-wgetcurl.sh - 在 WSL 里把上游 curl 8.22.0 / wget 1.6 源码静态编译,
# 产出 /home/jgzyes/wgetcurl-stage/bin/{curl,wget} 供 build-userland.sh 拷进
# initramfs(替代现有自研 http_client.c 版 curl/wget)。
#
# 前提: tarball 已下好(/home/jgzyes/{curl-8.22.0,wget-1.6}.tar.gz)或本脚本
# 自动下载。静态编译: -static + 静态 OpenSSL(复用 build-openssl.sh 的 stage)
# + 静态 zlib + glibc。产物零 .so 依赖(INTERP 段为空)。
#
# 用法: wsl -d Ubuntu-26.04 -e bash /mnt/f/Linux/Parlz/scripts/build-wgetcurl.sh
set -eu
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

BASE=/home/jgzyes
OSSL=$BASE/parlz-openssl/stage
OUT=$BASE/wgetcurl-stage

[ -f "$OSSL/lib/libssl.a" ] || { echo "缺 $OSSL/lib/libssl.a, 先跑 scripts/build-openssl.sh"; exit 1; }
ZLIB_A=""
for z in /usr/lib/x86_64-linux-gnu/libz.a /usr/lib/libz.a; do
  [ -f "$z" ] && ZLIB_A="$z" && break
done
if [ -z "$ZLIB_A" ]; then
  echo "  系统无静态 libz, 下载 zlib 源码编译..."
  [ -d "$BASE/zlib-src" ] || {
    cd "$BASE"; timeout 60 curl -sL -o zlib.tar.gz https://zlib.net/zlib-1.3.1.tar.gz; tar xzf zlib.tar.gz; mv zlib-1.3.1 zlib-src
  }
  make -C "$BASE/zlib-src" clean >/dev/null 2>&1 || true
  make -C "$BASE/zlib-src"
  ZLIB_A="$BASE/zlib-src/libz.a"
fi

rm -rf "$OUT"; mkdir -p "$OUT/bin"

# --- curl 8.22.0 (libcurl 静态 + 官方 src/main.c) ---
echo ">>> [1/2] 静态编译 curl 8.22.0 (OpenSSL + 静态)"
cd "$BASE"
[ -d curl-8.22.0 ] || tar xzf curl-8.22.0.tar.gz
CURL_SRC=$BASE/curl-8.22.0
CURL_INC="$CURL_SRC/include:$CURL_SRC/lib:$OSSL/include"

echo "  1a. 编译 libcurl 静态库..."
rm -rf "$CURL_SRC/lib/.o" ; mkdir -p "$CURL_SRC/lib/.o"
CC_FLAGS="-O2 -fno-pie -fno-stack-protector -D_GNU_SOURCE -fPIC -I$CURL_INC"
CURL_SRCS=""
for f in "$CURL_SRC/lib"/*.c; do
  b=$(basename "$f" .c)
  [ "$b" = "tool" ] && continue          # tool 是 CLI main 的一部分
  [ -f "$CURL_SRC/lib/.o/$b.o" ] || \
    gcc $CC_FLAGS -c "$f" -o "$CURL_SRC/lib/.o/$b.o" 2>/dev/null || \
    { echo "  curl 编译 $b.c 失败"; continue; }
done
ar rcs "$OUT/libcurl.a" "$CURL_SRC/lib"/.o/*.o
echo "  1b. 静态链接 curl CLI(src/main.c + libcurl.a)..."
gcc -O2 -fno-pie -fno-stack-protector -D_GNU_SOURCE \
    -I"$CURL_SRC/include" -I"$CURL_SRC" -I"$OSSL/include" \
    "$CURL_SRC/src/main.c" \
    -Wl,-static -L"$OUT" -lcurl \
    -L"$OSSL/lib" -lssl -lcrypto \
    -L"$(dirname "$ZLIB_A")" -lz \
    -o "$OUT/bin/curl"
file "$OUT/bin/curl" | head -1
"$OUT/bin/curl" --version | head -1

# --- wget 1.6 (官方源文件直链) ---
echo ">>> [2/2] 静态编译 wget 1.6 (OpenSSL + 静态)"
cd "$BASE"
[ -d wget-1.6 ] || tar xzf wget-1.6.tar.gz
WGET_SRC=$BASE/wget-1.6

# 生成 config.h: wget 需要, 从官方 config.h.in 填必要宏
if [ ! -f "$WGET_SRC/config.h" ]; then
  echo "  生成 $WGET_SRC/config.h..."
  cat > "$WGET_SRC/config.h" <<EOF
#define HAVE_GETOPT_LONG 1
#define HAVE_STDLIB_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETDB_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_WAIT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SIGNAL_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_TIME_H 1
#define HAVE_LIMITS_H 1
#define HAVE_FLOAT_H 1
#define HAVE_MATH_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GETOPT_H 1
#define HAVE_GETOPT_H 1
#define HAVE_PWD_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SGTTY_H 0
#define HAVE_TERMIOS_H 1
#define HAVE_XDR_XDR_H 0
#define HAVE_SGTTY_H 0
#define HAVE_SYS_STROPTS_H 0
#define HAVE_SYS_TIMEB_H 0
#define HAVE_NANOSLEEP 1
#define HAVE_SETENV 1
#define HAVE_FSYNC 1
#define HAVE_FTRUNCATE 1
#define HAVE_DUP 1
#define HAVE_DUP2 1
#define HAVE_GETOPT 1
#define HAVE_GETOPT_LONG 1
#define HAVE_SIGNAL 1
#define HAVE_SELECT 1
#define HAVE_TIME 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_FTIME 0
#define HAVE_STAT64 1
#define HAVE_FSTAT64 1
#define HAVE_FCNTL 1
#define HAVE_GETENV 1
#define HAVE_PUTENV 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_SYSLOG 0
#define HAVE_GETNAMEINFO 1
#define HAVE_GETADDRINFO 1
#define HAVE_INET_ATON 1
#define HAVE_INET_NTOP 1
#define HAVE_INET_PTON 1
#define HAVE_HTOON 1
#define HAVE_CRYPT 0
#define HAVE_DIRENT_H 1
#define HAVE_SYSLOG_H 0
#define HAVE_XLOCALE_H 0
#define HAVE_GETPWUID_R 1
#define HAVE_GETPWNAM_R 1
#define HAVE_GETGRGID_R 1
#define HAVE_GETGRNAM_R 1
#define HAVE_DOPRNTF 0
#define HAVE_ICONV 1
#define HAVE_ICONV_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_RESOLV_H 1
#define HAVE_ARPA_NAMESER_H 0
#define HAVE_GETPROTOBYNUMBER 1
#define HAVE_INET_RCMODEL 0
#define HAVE_XDR 0
#define HAVE_NANOSLEEP 1
#define HAVE_MKSTEMP 1
#define HAVE_MKDTEMP 1
#define HAVE_FTRUNCATE64 1
#define HAVE_POSIX_FADVISE 0
#define HAVE_GETRND 0
#define HAVE_ARPA_NAMESER 0
#define USE_SSL_OPENSSL
#define HAVE_OPENSSL
#define OPENSSL_LIB "-lssl -lcrypto"
#define OPENSSL_INC "-I$OSSL/include"
#define VERSION "1.6 (Parlz)"
#define PACKAGE_STRING "GNU Wget 1.6 (Parlz static)"
EOF
fi

WGET_SRCS=""
for f in "$WGET_SRC"/*.c; do
  b=$(basename "$f" .c)
  case $b in
    test*|example*) continue;;
  esac
  WGET_SRCS="$WGET_SRCS $f"
done

echo "  静态链接 wget..."
gcc -O2 -fno-pie -fno-stack-protector -D_GNU_SOURCE \
    -I"$WGET_SRC" -I"$OSSL/include" \
    $WGET_SRCS \
    -Wl,-static -L"$OSSL/lib" -lssl -lcrypto \
    -L"$(dirname "$ZLIB_A")" -lz \
    -o "$OUT/bin/wget"
file "$OUT/bin/wget" | head -1
"$OUT/bin/wget" --version | head -1

echo "=== 完成 ==="
ls -lh "$OUT/bin/"
file "$OUT/bin/curl" "$OUT/bin/wget"
