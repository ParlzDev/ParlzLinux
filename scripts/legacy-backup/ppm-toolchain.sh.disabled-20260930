#!/bin/sh
# ppm-toolchain.sh - PPM 包管理器(host 侧)工具
#
# 用法:
#   sh scripts/ppm-toolchain.sh build        构建全部 PPM 包(到 images/packages/)
#   sh scripts/ppm-toolchain.sh install-all   构建后,用 guest 的 ppm 把包全装上
#                                              (交互 QEMU 里跑 /bin/ppm install /mnt/ppm/*.ppm)
#
# .ppm = newc cpio 明文(PPMHEAD + 文件树 + TRAILER)。guest 的 /bin/ppm
# 直接解;host 侧用系统 cpio 可校验(070701 newc 格式)。
# 注:cpio 文本头格式是 newc 的 ASCII 变体,host cpio -o -H newc 生成
# 的是二进制版;为让 host 可复现,guest ppm 两者都能读(头部 magic 070701
# 一致,字段 ASCII hex)。

set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
PKG_ROOT="$ROOT/images/packages"
BUILD="$ROOT/images"
SRC="$ROOT/userland"

mkdir -p "$PKG_ROOT"

# 打包函数:把 $1(源目录内容)打成 $4(pkg 名),$5(版本),产物 $6(.ppm 路径)。
# 布局:$4 包内路径前缀 $2(相对 cpio 归档的路径),内容取自 $1。
pack_ppm() {
  pkgname="$1"; prefix="$2"; contentdir="$3"; version="$4"; out="$5"
  TMP=$(mktemp -d)
  STAGE="$TMP/stage"
  mkdir -p "$STAGE/$prefix"
  # 拷贝内容到 staging(prefix 下)
  if [ -d "$contentdir" ]; then
    cp -a "$contentdir"/. "$STAGE/$prefix/"
  fi
  # 写 PPMHEAD 元数据(newc cpio 文本头,magic 070701)
  (
    cd "$STAGE"
    printf "070701 00000001 0040755 00000000 00000000 00000001 00000000 %010x 00000000 00000000 00000000PPMHEAD " \
      $(printf "%s" "name: ${pkgname}
version: ${version}
desc: ${pkgname} package for Parlz" | wc -c) > "$TMP/head.bin"
    # PPMHEAD 条目后面跟负载(元数据文本)
    printf "name: ${pkgname}
version: ${version}
desc: ${pkgname} package for Parlz" >> "$TMP/head.bin"
    # pad 到 4
    sz=$(stat -c%s "$TMP/head.bin")
    pad=$(( (4 - ($sz % 4)) % 4 ))
    [ "$pad" -gt 0 ] && head -c "$pad" /dev/zero >> "$TMP/head.bin"
    cat "$TMP/head.bin" > "$out.tmp"
    # 文件树 newc 条目
    find "$STAGE/$prefix" -mindepth 1 \( -type f -o -type d \) | LC_ALL=C sort | \
    while IFS= read -r p; do
      rel="${p#"$STAGE/"}"
      if [ -d "$p" ]; then
        mode=0040755
        sz=0
      else
        # 可执行?给 0755,否则 0644
        if [ -x "$p" ]; then mode=0100755; else mode=0100644; fi
        sz=$(stat -c%s "$p")
      fi
      ino=$(( (sz + 1) % 65535 + 1 ))
      printf "070701 %08x %08x 00000000 00000000 00000001 00000000 %010x 00000000 00000000 00000000%s " \
        "$ino" "$mode" "$sz" "$rel" >> "$out.tmp"
      # pad 头部到 4
      hdrsz=$(printf "070701 %08x %08x 00000000 00000000 00000001 00000000 %010x 00000000 00000000 00000000%s " \
        "$ino" "$mode" "$sz" "$rel" | wc -c)
      # 上面已经把 header 写进去了,现在补 4 字节对齐
      cur=$(stat -c%s "$out.tmp")
      pad=$(( (4 - ($cur % 4)) % 4 ))
      [ "$pad" -gt 0 ] && head -c "$pad" /dev/zero >> "$out.tmp"
      if [ -f "$p" ]; then
        cat "$p" >> "$out.tmp"
        cur=$(stat -c%s "$out.tmp")
        pad=$(( (4 - ($cur % 4)) % 4 ))
        [ "$pad" -gt 0 ] && head -c "$pad" /dev/zero >> "$out.tmp"
      fi
    done
    # TRAILER
    printf "070701 00000000 00000000 00000000 00000000 00000001 00000000 %010x 00000000 00000000 00000000TRAILER/// " 0 >> "$out.tmp"
    cur=$(stat -c%s "$out.tmp")
    pad=$(( (4 - ($cur % 4)) % 4 ))
    [ "$pad" -gt 0 ] && head -c "$pad" /dev/zero >> "$out.tmp"
  )
  mv "$out.tmp" "$out"
  rm -rf "$TMP"
  echo "PPM 包: $out ($(stat -c%s "$out") bytes)"
}

cmd="${1:-build}"

if [ "$cmd" = "build" ] || [ "$cmd" = "all" ]; then
  echo ">>> 构建 PPM 包 -> $PKG_ROOT"

  # --- paze-ssl-ssh:静态库 + pazessl + pssh 入口(OpenSSL/SSH 替代)---
  # 源:TLS-SSH paze.a(build-userland 已生成 $BUILD/../paze.a 或 $ROOT/paze.a)
  PAZE_A="$ROOT/paze.a"
  if [ ! -f "$PAZE_A" ] && [ -d /home/jgzyes/parlz-userland ]; then
    PAZE_A="/home/jgzyes/parlz-userland/paze.a"
  fi
  if [ -f "$PAZE_A" ]; then
    STAGE_D="$PKG_ROOT/stage-paze"
    rm -rf "$STAGE_D"; mkdir -p "$STAGE_D/lib" "$STAGE_D/bin"
    cp "$PAZE_A" "$STAGE_D/lib/paze.a"
    # 复制 pazessl / pssh 源码(编译在 build-userland,这里只带 .a 与入口源)
    if [ -d "$ROOT/TLS-SSH/apps/pazessl" ]; then
      cp "$ROOT/TLS-SSH/apps/pazessl/main.c" "$STAGE_D/bin/pazessl.c"
    fi
    if [ -d "$ROOT/TLS-SSH/apps/ssh" ]; then
      cp "$ROOT/TLS-SSH/apps/ssh/main.c" "$STAGE_D/bin/pssh.c"
    fi
    pack_ppm "paze-ssl-ssh" "lib" "$STAGE_D/lib" "1.0" "$PKG_ROOT/paze-ssl-ssh.ppm"
    pack_ppm "paze-ssl-ssh-bin" "bin" "$STAGE_D/bin" "1.0" "$PKG_ROOT/paze-ssl-ssh-bin.ppm"
    rm -rf "$STAGE_D"
  else
    echo "    (跳过 paze:未找到 paze.a,先跑 build-userland.sh)"
  fi

  # --- tree / 文本工具包:把 userland 已编译二进制打进包 ---
  BINTOOLS="$PKG_ROOT/stage-tools"
  rm -rf "$BINTOOLS"; mkdir -p "$BINTOOLS/bin"
  if [ -d /home/jgzyes/parlz-userland/build/bin ]; then
    for t in tree wc sort sed awk ps df ln mv grep head tail; do
      [ -f "/home/jgzyes/parlz-userland/build/bin/$t" ] && \
        cp "/home/jgzyes/parlz-userland/build/bin/$t" "$BINTOOLS/bin/$t"
    done
  fi
  if ls "$BINTOOLS/bin/" 2>/dev/null | grep -q .; then
    pack_ppm "text-tools" "bin" "$BINTOOLS/bin" "1.0" "$PKG_ROOT/text-tools.ppm"
  fi
  rm -rf "$BINTOOLS"

  # --- frpc / openvpn:编译好的二进制打进 .ppm ---
  # 这两个是本仓库 userland 的 Paze 工具(非外部 Go/系统包),直接从
  # build/bin 取已编译产物打包;若 build 目录缺则跳过(先跑 build-userland)。
  BUILDBIN="/home/jgzyes/parlz-userland/build/bin"
  VSTAGE="$PKG_ROOT/stage-venv"; rm -rf "$VSTAGE"; mkdir -p "$VSTAGE/bin" "$VSTAGE/sbin"
  if [ -d "$BUILDBIN" ]; then
    [ -f "$BUILDBIN/frpc" ] && cp "$BUILDBIN/frpc" "$VSTAGE/bin/frpc"
    [ -f "$BUILDBIN/openvpn" ] && cp "$BUILDBIN/openvpn" "$VSTAGE/bin/openvpn"
    [ -f "$BUILDBIN/pazessl" ] && cp "$BUILDBIN/pazessl" "$VSTAGE/bin/pazessl"
    [ -f "$BUILDBIN/pssh" ] && cp "$BUILDBIN/pssh" "$VSTAGE/bin/pssh"
    [ -f "$BUILDBIN/ppm" ] && cp "$BUILDBIN/ppm" "$VSTAGE/bin/ppm"
    [ -f "$BUILDBIN/tree" ] && cp "$BUILDBIN/tree" "$VSTAGE/bin/tree"
  fi
  if ls "$VSTAGE/bin/" 2>/dev/null | grep -q .; then
    pack_ppm "vpn-tools" "bin" "$VSTAGE/bin" "1.0" "$PKG_ROOT/vpn-tools.ppm"
  fi
  rm -rf "$VSTAGE"

  echo ">>> PPM 包构建完成"
  ls -la "$PKG_ROOT"/*.ppm 2>/dev/null | awk '{print "  ", $5, $9}'
elif [ "$cmd" = "install-all" ]; then
  echo ">>> 交互:在 QEMU 内跑 /bin/ppm install /mnt/ppm/*.ppm"
  echo "    1) sh scripts/run-iso.sh(挂 ISO)"
  echo "    2) guest 里:mkdir -p /mnt/ppm && mount /dev/sr0 /mnt/ppm"
  echo "       把本机的 $PKG_ROOT/*.ppm 通过 iso 或 9p 传入 /mnt/ppm/"
  echo "       /bin/ppm install /mnt/ppm/*.ppm"
else
  echo "用法: $0 {build|all|install-all}"
  exit 1
fi
