#!/bin/bash
# vendor-licenses.sh - 把交付物里每个上游组件的许可证**全文**收进
#   third_party/licenses/<组件>/, 供 build-userland.sh 原样烘进 rootfs 的
#   /usr/share/licenses/<组件>/, 以及 gen-fatboot.sh 放进引导分区。
#
# 为什么要 vendor(存进仓库)而不是构建时从宿主现拷:
#   "分发二进制却没带许可证文本"是 GPLv2 §3 的硬违规(引导分区上那个
#   vmlinuz 就是改过的 Linux 内核)。要是许可证文本依赖构建机的
#   /usr/share/common-licenses, 换一台机器就悄悄少文件、没人报错。
#   存进仓库 = 交付内容确定, 且能进 git 审查。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c \
#         "bash /mnt/f/Linux/Parlz/scripts/vendor-licenses.sh"
set -eu

P=/mnt/f/Linux/Parlz
D="$P/third_party/licenses"
CL=/usr/share/common-licenses
[ -d "$CL" ] || { echo "缺 $CL(Ubuntu/Debian 的 common-licenses), 换宿主请先补齐来源"; exit 1; }
mkdir -p "$D"

grab(){ # $1=组件 $2=目标文件名 $3=来源路径
  mkdir -p "$D/$1"
  cp -aL "$3" "$D/$1/$2"
  printf '    %-13s %-14s %7s 字节 <- %s\n' "$1" "$2" "$(stat -c%s "$D/$1/$2")" "$3"
}

echo ">>> 上游许可证文本 -> third_party/licenses/"
grab linux-kernel COPYING      "$CL/GPL-2"      # GPLv2-only(Linux 内核)
grab busybox      COPYING      "$CL/GPL-2"      # GPLv2
grab syslinux     COPYING      "$CL/GPL-2"      # GPL-2.0-or-later
grab bash         COPYING      "$CL/GPL-3"      # GPLv3-or-later
grab nano         COPYING      "$CL/GPL-3"
grab wget         COPYING      "$CL/GPL-3"
grab glibc        COPYING.LIB  "$CL/LGPL-2.1"   # LGPL-2.1(静态链接, §6 要给可重链接材料)
grab openssl      LICENSE.txt  "$CL/Apache-2.0"
# curl 的许可证是它自己的(MIT/X 系), 取宿主包里的版权文件全文
[ -f /usr/share/doc/curl/copyright ] && grab curl COPYRIGHT /usr/share/doc/curl/copyright \
  || echo "    WARNING: 宿主没有 /usr/share/doc/curl/copyright(apt-get install -y curl)"

# miniz: 许可证写在源码头注释里(userland/miniz_tinfl.c), 抽成独立文本
# (那个头是 MIT 风格全文, 不是 "/* miniz ..." 开头 —— 按"从第 1 行的 /* 到
#  第一个 */"抽, 抽不到再退到手抄声明, 并且两种情况都报出来。)
mkdir -p "$D/miniz"
awk 'NR==1 && /^\/\*/{f=1} f{print} f&&/\*\//{exit}' "$P/userland/miniz_tinfl.c" > "$D/miniz/LICENSE" || true
if [ "$(wc -c < "$D/miniz/LICENSE")" -lt 200 ]; then
  # 抽不到就退回手抄的最小声明(注明出处), 别让交付盘里缺这一份
  cat > "$D/miniz/LICENSE" <<'EOF'
miniz (tinfl 子集) — 随 userland/miniz_tinfl.c 一并分发。
来源: Rich Geldreich <richgel99@gmail.com> 的 miniz 项目, MIT 风格许可
(The Software is provided "as is", without warranty of any kind)。
全文见该源文件的头部注释。
EOF
fi
printf '    %-13s %-14s %7s 字节 <- userland/miniz_tinfl.c 头注释\n' miniz LICENSE "$(stat -c%s "$D/miniz/LICENSE")"

# Parlz 自己的两份授权文本不进 vendor 目录(仓库根就是唯一来源),
# build-userland.sh 直接从仓库根拷, 避免出现两份会漂移的副本。

# ---- 索引: 组件 -> 许可证标识 -> 镜像内路径 ----
cat > "$D/README" <<'EOF'
third_party/licenses/<组件>/ = 交付物里上游组件的许可证**全文**。

build-userland.sh 把每个子目录原样拷进 rootfs 的 /usr/share/licenses/<组件>/,
gen-fatboot.sh 另把 GPLv2 全文与一份分层授权说明放进引导分区(vmlinuz 所在处),
build-iso.sh 放进 ISO 根。分层授权的完整说明见仓库根的 LICENSE。

组件与许可证标识(与 web/license.html 同一份清单):
  linux-kernel  GPL-2.0-only (+ Linux-syscall-note)   linux-7.2.5/ 整树
  busybox       GPL-2.0                               /sbin/busybox
  syslinux      GPL-2.0-or-later                      引导分区 mbr.bin/VBR/ldlinux.*
  bash          GPL-3.0-or-later                      /bin/bash
  nano          GPL-3.0-or-later                      /bin/nano(core.pm)
  wget          GPL-3.0-or-later                      /bin/wget
  glibc         LGPL-2.1-or-later                     静态链进所有用户空间命令
  openssl       Apache-2.0                            PazeSSL/curl/wget 的 TLS 后端
  curl          curl(MIT/X 系)                        /bin/curl
  miniz         Miniz license(MIT 风格)               userland/miniz_tinfl.c

新增上游件时: 在 build 脚本里跑一次本脚本补文本, 并在仓库根 LICENSE 的表格里加一行。
EOF

# ---- 自检: 交付链上每个组件都得有文本, 少一个就失败 ----
MISS=""
for c in linux-kernel busybox syslinux bash nano wget glibc openssl curl miniz; do
  [ -d "$D/$c" ] && [ -n "$(ls -A "$D/$c" 2>/dev/null)" ] || MISS="$MISS $c"
done
if [ -n "$MISS" ]; then echo "!! third_party/licenses 缺组件文本:$MISS"; exit 1; fi
echo "    自检: 10 个组件的许可证文本齐(README 索引已生成)"

# ---- 打印 web/system.js 里 LICENSES 字面量的正确值 ----
# 演示站要把 /usr/share/licenses 每个条目的**字节数**照真机写死一份,
# 手写就会漂(bash 那份是早先手工存的 GPLv3, 与宿主 common-licenses 的
# 版本差 2 字节, 结果演示里报的尺寸和盘上的对不上)。这里现算现出, 贴过去即可,
# web-demo-test.js 有一条判据逐条比对两边, 漂了直接红。
echo "    web/system.js 里应写成:"
echo "      const LICENSES = ["
printf '      ["%s", "%s", %s],\n' parlz PARLZ.LICENSE "$(stat -c%s "$P/PARLZ.LICENSE")"
printf '      ["%s", "%s", %s],\n' parlz LICENSE "$(stat -c%s "$P/LICENSE")"
for c in linux-kernel busybox syslinux bash nano wget glibc openssl curl miniz; do
  for f in "$D/$c"/*; do
    printf '      ["%s", "%s", %s],\n' "$c" "$(basename "$f")" "$(stat -c%s "$f")"
  done
done
echo "      ];"
echo "      const LICENSES_README_SIZE = $(stat -c%s "$D/README");"
echo "      const MEDIA_LICENSE_SIZE = $(stat -c%s "$D/PARLZ-MEDIA-LICENSE.txt");"
echo "=== vendor-licenses OK: $D ==="
