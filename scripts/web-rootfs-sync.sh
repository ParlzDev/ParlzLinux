#!/bin/sh
# 把真机 rootfs 里的真文件（非软链）同步到 web/rootfs/，供官网演示终端按需取字节：
#   ./rootfs/bin/cat 就是真机上那个 820624 字节的 ELF，file/sha256sum/od/strings 都看它。
# 软链不复制（演示端自己按真机目标建链）。
#
#   sh scripts/web-rootfs-sync.sh            # 同步（源为 WSL 里的构建产物）
#   ROOT=<别的 rootfs> sh scripts/web-rootfs-sync.sh
set -eu
ROOT=${ROOT:-/home/jgzyes/parlz-userland/root}
OUT=${OUT:-/mnt/f/Linux/Parlz/web/rootfs}

[ -d "$ROOT" ] || { echo "找不到 rootfs: $ROOT" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"
n=0; bytes=0
copy() {
  src="$1"; rel="$2"
  [ -f "$src" ] || return 0
  [ -L "$src" ] && return 0                      # 软链跳过
  mkdir -p "$OUT/$(dirname "$rel")"
  cp -p "$src" "$OUT/$rel"
  sz=$(stat -c%s "$src")
  n=$((n + 1)); bytes=$((bytes + sz))
}
for f in "$ROOT"/bin/* "$ROOT"/sbin/* "$ROOT"/usr/bin/* "$ROOT"/usr/sbin/* \
         "$ROOT"/usr/local/bin/* "$ROOT"/init "$ROOT"/install.d "$ROOT"/nettest.sh \
         "$ROOT"/parlz/banner "$ROOT"/LICENSE.TXT \
         "$ROOT"/usr/share/licenses/README "$ROOT"/usr/share/licenses/*/*; do
  [ -e "$f" ] || continue
  rel=${f#"$ROOT"/}
  copy "$f" "$rel"
done

echo "同步完成: $n 个真文件, $bytes 字节 ($(awk "BEGIN{printf \"%.1f\", $bytes/1048576}") MiB) → $OUT"
echo "抽样自检:"
for probe in bin/cat bin/bash sbin/busybox bin/parlz-sh; do
  [ -f "$OUT/$probe" ] && printf '  %-16s %8s  %s\n' "$probe" "$(stat -c%s "$OUT/$probe")" "$(head -c4 "$OUT/$probe" | od -An -c | tr -s ' ')"
done
