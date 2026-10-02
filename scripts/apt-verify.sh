#!/bin/sh
# apt-verify.sh - 自研 apt 前端的宿主侧回归(秒级, 不起 QEMU)。
#
# 仓库是**真仓库**: .deb 由宿主 dpkg-deb 造, 索引由宿主 apt-ftparchive 生成
# (Packages 段落、SHA256、Size、Filename 全是上游工具写的), 再用宿主 python3
# 起一个静态 http 站点当镜像。apt 走自己的下载/解析/依赖求解, 最后 fork+execv
# 调本仓库的 dpkg 落盘。
#
# 判据全是终态: 依赖闭包里的包**全都**装上、坏依赖一个文件都不落、
# 索引声明的 SHA256 与实得不符时必须删缓存且不交给 dpkg、
# 没标 trusted=yes 的源不许装、反向依赖没清完不许删。
#
#   sh scripts/apt-verify.sh [apt 二进制] [dpkg 二进制]
set -u
T=/tmp/parlz-apt-test
APT=${1:-/mnt/f/Linux/Parlz/userland/build/bin/apt}
DPKG=${2:-/mnt/f/Linux/Parlz/userland/build/bin/dpkg}
PORT=${PORT:-18771}
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'PASS  %s\n' "$1"; }
no()  { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }
chk() { if eval "$2"; then ok "$1"; else no "$1  [谓词: $2]"; fi }
run() { _d="$1"; _e="$2"; shift 2
    "$@" >"$T/out.log" 2>&1; _r=$?
    if [ "$_e" = 0 ] && [ "$_r" = 0 ]; then ok "$_d"
    elif [ "$_e" != 0 ] && [ "$_r" != 0 ]; then ok "$_d (按预期失败 rc=$_r)"
    else no "$_d (rc=$_r, 期望 $([ "$_e" = 0 ] && echo 0 || echo 非0))"
         sed 's/^/      | /' "$T/out.log" | tail -8; fi; }

for f in "$APT" "$DPKG"; do
    [ -x "$f" ] || { echo "!! 缺可执行文件: $f"; exit 1; }
done
SRCDIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)     # 仓库根
for t in dpkg-deb apt-ftparchive python3; do
    command -v $t >/dev/null 2>&1 || { echo "!! 宿主缺 $t —— 造不出真仓库"; exit 1; }
done

rm -rf "$T"; mkdir -p "$T"
SRC=$T/src; REPO=$T/repo
mkdeb() { # mkdeb <包名> <版本> <Depends 串> <内容标记>
    d=$SRC/$1; rm -rf "$d"; mkdir -p "$d/bin" "$d/etc/$1" "$d/DEBIAN"
    printf '#!/bin/sh\necho %s=%s\n' "$1" "$4" > "$d/bin/$1"
    chmod 755 "$d/bin/$1"
    printf 'conf-for-%s\n' "$1" > "$d/etc/$1/$1.conf"
    { printf 'Package: %s\nVersion: %s\nArchitecture: all\n' "$1" "$2"
      printf 'Maintainer: Parlz <pkg@parlz.com>\nInstalled-Size: 3\n'
      printf 'Priority: optional\nSection: misc\n'
      [ -n "$3" ] && printf 'Depends: %s\n' "$3"
      printf 'Description: %s test payload\n Long description for %s.\n' "$1" "$1"
    } > "$d/DEBIAN/control"
    dpkg-deb -Zgzip --build "$d" "$REPO/pool/$1_$2_all.deb" >/dev/null \
        || { echo "!! dpkg-deb $1 失败"; exit 1; }
}
mkdir -p "$REPO/pool" "$REPO/dists/stable/main/binary-amd64" \
         "$REPO/dists/stable/main/binary-all"
mkdeb libfoo         1.2 ""                    LIBFOOTAG
mkdeb appfoo         2.0 "libfoo (>= 1.0)"     APPFOOTAG
mkdeb ortool         1.0 "libfoo | missinglib" ORTAG
mkdeb badbin         1.0 "nosuchpackage-at-all" BADTAG
mkdeb libcritical    3.1 ""                    CRITTAG
mkdeb needs-critical 1.0 "libcritical"         NEEDTAG

# 索引由上游工具生成: Filename 相对仓库根, SHA256/Size 都是真值
( cd "$REPO" && apt-ftparchive packages pool > dists/stable/main/binary-all/Packages ) \
    || { echo "!! apt-ftparchive 失败"; exit 1; }
cp "$REPO/dists/stable/main/binary-all/Packages" \
   "$REPO/dists/stable/main/binary-amd64/Packages"
gzip -kf "$REPO/dists/stable/main/binary-amd64/Packages"
( cd "$REPO" && apt-ftparchive release dists/stable > dists/stable/Release )

echo "=== 0) 底座: SHA-256 与上游逐字节一致(整条完整性判据都靠它) ==="
KAT=$T/shakat.c
mkdir -p "$T"
cat > "$KAT" <<'CEOF'
#include <stdio.h>
#include "pkgcore.h"
int main(int argc, char **argv)
{
    char h[65];
    pc_sha256_hex("", 0, h);
    printf("empty %s\n", h);
    pc_sha256_hex("abc", 3, h);
    printf("abc %s\n", h);
    if (argc > 1 && pc_file_sha256_hex(argv[1], h) == 0)
        printf("file %s\n", h);
    return 0;
}
CEOF
head -c 100000 /dev/urandom > "$T/kat.bin"
if gcc -O2 -I/mnt/f/Linux/Parlz/userland "$KAT" \
        /mnt/f/Linux/Parlz/userland/pkgcore.c \
        /mnt/f/Linux/Parlz/userland/miniz_tinfl.c -o "$T/shakat" 2>"$T/kat.log"; then
    e=$("$T/shakat" | awk '/^empty/{print $2}')
    r=$(printf '' | sha256sum | cut -d' ' -f1)
    chk "空串摘要与 sha256sum 一致" "[ \"$e\" = \"$r\" ]"
    a=$("$T/shakat" | awk '/^abc/{print $2}')
    r2=$(printf 'abc' | sha256sum | cut -d' ' -f1)
    chk "\"abc\" 摘要一致" "[ \"$a\" = \"$r2\" ]"
    f=$("$T/shakat" "$T/kat.bin" | awk '/^file/{print $2}')
    r3=$(sha256sum "$T/kat.bin" | cut -d' ' -f1)
    chk "100KB 文件摘要一致(覆盖多块+填充边界)" "[ \"$f\" = \"$r3\" ]"
    for n in 55 56 57 63 64 65 119 120 127 128; do
        head -c $n /dev/urandom > "$T/k$n.bin"
        x=$("$T/shakat" "$T/k$n.bin" | awk '/^file/{print $2}')
        y=$(sha256sum "$T/k$n.bin" | cut -d' ' -f1)
        [ "$x" = "$y" ] || no "长度 $n 的摘要不一致"
    done
    ok "填充边界长度(55/56/57/63/64/65/119/120/127/128)全部一致"
else
    no "SHA-256 自检程序编不出来(不能验摘要)"
    sed 's/^/      | /' "$T/kat.log" | head -5
fi

echo "=== 0b) fixture: 确实是真 Debian 索引 ==="
PKGS=$REPO/dists/stable/main/binary-amd64/Packages
chk "Packages 段落数 = 造的包数(6)" \
    "[ \$(grep -c '^Package: ' '$PKGS') -eq 6 ]"
chk "索引带 SHA256 与 Filename 字段(上游写的)" \
    "grep -q '^SHA256: [0-9a-f]\{64\}' '$PKGS' && grep -qx 'Filename: pool/appfoo_2.0_all.deb' '$PKGS'"
chk "gz 与未压缩两种都在" \
    "[ -f '${PKGS}.gz' ] && [ -f '$REPO/dists/stable/main/binary-all/Packages' ]"

python3 -m http.server $PORT --directory "$REPO" --bind 127.0.0.1 \
    >"$T/httpd.log" 2>&1 &
HTTPD=$!
trap 'kill $HTTPD 2>/dev/null' EXIT INT TERM
i=0
while ! python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(1)
sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)" 2>/dev/null; do
    i=$((i+1))
    if [ $i -gt 40 ]; then echo "!! 本地站点起不来"; cat "$T/httpd.log"; exit 1; fi
    sleep 0.25
done
ok "本地站点在 $PORT 上应答"

R=$T/root; A=$T/db
mkroot() { rm -rf "$R" "$A"; mkdir -p "$R/bin" "$R/etc/apt" "$R/var/lib/dpkg" \
                  "$R/var/lib/apt/lists" "$R/var/cache/apt/archives" "$R/tmp" "$A"; }
T_APT() { "$APT" --root "$R" --dpkg "$DPKG" "$@"; }
T_DPKG() { "$DPKG" --root "$R" "$@"; }

echo "=== 1) update 与查询 ==="
mkroot
cat > "$R/etc/apt/sources.list" <<EOS
deb [trusted=yes] http://127.0.0.1:$PORT stable main
EOS
run "apt update 成功" 0 T_APT update
chk "缓存里落了索引(每个架构一份, 共 2 份)" \
    "[ \$(ls '$R/var/lib/apt/lists' | grep -c '_Packages\$') -eq 2 ]"
chk "两份缓存合起来含 12 个段落(6 包 × 2 架构)" \
    "[ \$(cat '$R'/var/lib/apt/lists/*_Packages | grep -c '^Package: ') -eq 12 ]"
chk "apt list 打出未装的 appfoo" \
    "T_APT list | grep -qx 'appfoo/not-installed 2.0'"
chk "apt policy 打出源地址" \
    "T_APT policy | grep -q 'http://127.0.0.1:$PORT'"
chk "apt search foo 命中 appfoo" \
    "T_APT search foo | grep -q '^appfoo - '"
chk "apt show appfoo 打出 Depends 与 SHA256" \
    "T_APT show appfoo | grep -q '^Depends: libfoo' && T_APT show appfoo | grep -q '^SHA256: '"
chk "查询动作不装任何东西" \
    "[ ! -e '$R/bin/appfoo' ]"

echo "=== 2) 依赖闭包安装(终态: 两个包的文件都在) ==="
run "apt install appfoo" 0 T_APT -y install appfoo
chk "被依赖的 libfoo 也装了" \
    "[ -x '$R/bin/libfoo' ] && grep -qx 'echo libfoo=LIBFOOTAG' '$R/bin/libfoo'"
chk "要的 appfoo 装了" \
    "[ -x '$R/bin/appfoo' ] && grep -qx 'echo appfoo=APPFOOTAG' '$R/bin/appfoo'"
chk "dpkg 状态库里两个都是 installed" \
    "T_DPKG -l | grep -q '^ii appfoo' && T_DPKG -l | grep -q '^ii libfoo'"
chk "apt list --installed 两条都在" \
    "T_APT list --installed | grep -q '^appfoo/installed' && T_APT list --installed | grep -q '^libfoo/installed'"
chk "apt policy 显示已装版本" \
    "T_APT policy appfoo | grep -q '已装: 2.0'"
chk "下载缓存里有那个 .deb" \
    "[ -f '$R/var/cache/apt/archives/appfoo_2.0_all.deb' ]"
run "apt clean 清缓存" 0 T_APT clean
chk "缓存目录空了" "[ -z \"\$(ls -A '$R/var/cache/apt/archives')\" ]"

echo "=== 3) 或关系依赖: 满足一项就够 ==="
run "apt install ortool(Depends: libfoo | missinglib)" 0 T_APT -y install ortool
chk "ortool 装上了" "[ -x '$R/bin/ortool' ]"
chk "没为 missinglib 报错(或关系已满足)" "T_DPKG -l | grep -q '^ii ortool'"

echo "=== 4) 依赖无法满足: 整体失败且什么都没落 ==="
run "apt install badbin(依赖不存在)要失败" N T_APT -y install badbin
chk "badbin 没落盘" "[ ! -e '$R/bin/badbin' ]"
chk "错误里点名缺的那个依赖" \
    "T_APT -y install badbin 2>&1 | grep -qi 'nosuchpackage-at-all'"
chk "失败的安装没写状态库条目" "! T_DPKG -l | grep -q badbin"

echo "=== 5) 完整性闸门: 索引说的 SHA256/Size 与实得不符就不许装 ==="
# (a) 等长改一个字节 -> Size 对得上, 只能靠 SHA256 这一关抓住
cp "$REPO/pool/needs-critical_1.0_all.deb" "$T/nc.orig.deb"
python3 - "$REPO/pool/needs-critical_1.0_all.deb" <<'PY'
import sys
p = sys.argv[1]
b = bytearray(open(p, "rb").read())
i = len(b) // 2
b[i] ^= 0xFF                      # 长度不变, 内容变了
open(p, "wb").write(bytes(b))
print("flipped one byte in place (size unchanged)")
PY
rm -f "$R/var/cache/apt/archives/needs-critical_1.0_all.deb"
run "被改过内容的包必须被拒" N T_APT -y install needs-critical
chk "被改过的包一个文件都没落" "[ ! -e '$R/bin/needs-critical' ]"
chk "拒绝理由点名 SHA256 不符" \
    "T_APT -y install needs-critical 2>&1 | grep -qi 'SHA256'"
chk "坏包没留在下载缓存里" \
    "[ ! -f '$R/var/cache/apt/archives/needs-critical_1.0_all.deb' ]"
chk "状态库里也没有它" "! T_DPKG -l | grep -q needs-critical"
chk "它依赖的 libcritical 也没被顺手装上(整体失败)" \
    "[ ! -e '$R/bin/libcritical' ]"
# (b) 加长一字节 -> Size 这一关就该先抓住
printf 'TAMPERED-EXTRA-BYTE\n' >> "$REPO/pool/needs-critical_1.0_all.deb"
rm -f "$R/var/cache/apt/archives/needs-critical_1.0_all.deb"
run "被加长的包也必须被拒" N T_APT -y install needs-critical
chk "拒绝理由点名大小不符" \
    "T_APT -y install needs-critical 2>&1 | grep -qi '大小不符'"
cp "$T/nc.orig.deb" "$REPO/pool/needs-critical_1.0_all.deb"
rm -f "$R/var/cache/apt/archives/needs-critical_1.0_all.deb"
run "修好之后同一个包就能装了(证明上面两关不是恒失败)" 0 \
    T_APT -y install needs-critical

echo "=== 6) 信任闸门: 没标 trusted=yes 的源不装 ==="
mkroot
cat > "$R/etc/apt/sources.list" <<EOS
deb http://127.0.0.1:$PORT stable main
EOS
run "未信任源: update 本身能跑" 0 T_APT update
run "未信任源: install 必须被拒" N T_APT -y install libfoo
chk "拒绝理由点名 trusted 与不验签名" \
    "T_APT -y install libfoo 2>&1 | grep -qi 'trusted'"
chk "未信任源的包没落盘" "[ ! -e '$R/bin/libfoo' ]"
run "--allow-unauthenticated 明示放行才装" 0 \
    T_APT --allow-unauthenticated -y install libfoo
chk "放行后确实装上了" "[ -x '$R/bin/libfoo' ]"

echo "=== 7) 反向依赖保护 ==="
mkroot
cat > "$R/etc/apt/sources.list" <<EOS
deb [trusted=yes] http://127.0.0.1:$PORT stable main
EOS
T_APT update >/dev/null 2>&1
run "装 needs-critical(它 Depends libcritical)" 0 T_APT -y install needs-critical
chk "两个都装上了" \
    "[ -x '$R/bin/needs-critical' ] && [ -x '$R/bin/libcritical' ]"
run "单删被依赖的 libcritical 必须被拒" N T_APT -y remove libcritical
chk "被拒后 libcritical 还在" "[ -x '$R/bin/libcritical' ]"
chk "拒绝理由点名依赖方 needs-critical" \
    "T_APT -y remove libcritical 2>&1 | grep -q 'needs-critical'"
run "两个一起删就放行" 0 T_APT -y remove needs-critical libcritical
chk "删完两个文件都没了" \
    "[ ! -e '$R/bin/needs-critical' ] && [ ! -e '$R/bin/libcritical' ]"
chk "状态库里两个都不是 installed" \
    "! T_DPKG -l | grep -E '^ii (needs-critical|libcritical)' | grep -q ."

echo "=== 8) 源不可达 / 索引为空: 要报错, 不能'什么都没装还打成功' ==="
mkroot
cat > "$R/etc/apt/sources.list" <<EOS
deb [trusted=yes] http://127.0.0.1:1 stable main
EOS
run "源不可达时 update 返回非 0" N T_APT update
run "索引为空时 install 返回非 0" N T_APT -y install libfoo
chk "错误里提示先 apt update" \
    "T_APT -y install libfoo 2>&1 | grep -qi 'update'"

echo "=== RESULT: pass=$pass fail=$fail ==="
kill $HTTPD 2>/dev/null
[ "$fail" = 0 ] || exit 1
[ -n "${KEEP:-}" ] || rm -rf "$T"
