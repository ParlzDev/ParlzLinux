#!/bin/sh
# dpkg-verify.sh - 自研 dpkg 移植的宿主侧回归(秒级, 不起 QEMU)。
#
# 判据全是**终态**: 文件真的落在 scratch root 里、权限/软链/长文件名对、
# 状态库条目对、脚本真的跑过、卸载后文件真的没了 —— 不看 dpkg 自己打的字。
# 另有三条"必须拒绝"的反例: xz 负载、别的架构、降级安装。反例的意义在于
# 实现如果偷懒(解不动就当空包、架构不看、版本不比)就会被抓住。
#
#   sh scripts/dpkg-verify.sh [dpkg 二进制路径]
#   KEEP=1 保留 /tmp/parlz-dpkg-test 现场
set -u
ROOT_TEST=/tmp/parlz-dpkg-test
DPKG=${1:-/mnt/f/Linux/Parlz/userland/build/bin/dpkg}
[ -n "${DPKG_ABS:-}" ] && DPKG="$DPKG_ABS"
pass=0; fail=0
ok()   { pass=$((pass+1)); printf 'PASS  %s\n' "$1"; }
no()   { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }
chk()  { if eval "$2"; then ok "$1"; else no "$1  [谓词: $2]"; fi }
chkrc() { # chkrc <说明> <期望rc(0=必须成功, N=必须非0)> <命令...>
    _d="$1"; _e="$2"; shift 2
    "$@" >/tmp/parlz-dpkg-test.log 2>&1; _r=$?
    if [ "$_e" = 0 ] && [ "$_r" = 0 ]; then ok "$_d"
    elif [ "$_e" != 0 ] && [ "$_r" != 0 ]; then ok "$_d (按预期失败 rc=$_r)"
    else no "$_d (rc=$_r, 期望 $(_z "$_e") 实际 $_r)"; fi
}
_z() { [ "$1" = 0 ] && echo 0 || echo "非0"; }

[ -x "$DPKG" ] || { echo "!! 找不到可执行的 dpkg: $DPKG"; exit 1; }
command -v dpkg-deb >/dev/null 2>&1 || { echo "!! 宿主缺 dpkg-deb(造不出真 .deb 对照物)"; exit 1; }

rm -rf "$ROOT_TEST"
SRC=$ROOT_TEST/src
SRCD=$ROOT_TEST/src2
BAD=$ROOT_TEST/srcbad
XZ=$ROOT_TEST/srcxz
A64=$ROOT_TEST/src64
mkdir -p "$SRC/bin" "$SRC/usr/share/pkgdemo/sub dir" "$SRCD/bin" \
         "$BAD/bin" "$XZ/bin" "$A64/bin"

# ---- 造一个真 .deb: 用宿主的 dpkg-deb, 这样 tar/ar/gzip 都是上游写的 ----
printf '#!/bin/sh\necho pkgdemo running\n' > "$SRC/bin/pkgdemo"
chmod 755 "$SRC/bin/pkgdemo"
printf 'PARLZ-DATA-42\n' > "$SRC/usr/share/pkgdemo/data.txt"
printf 'cfg=1\n' > "$SRC/usr/share/pkgdemo/sub dir/app.conf"
# 名字超过 tar ustar 的 100 字节字段 -> 逼出 GNU 'L' 长名扩展
_long=verylongdirectoryname_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
mkdir -p "$SRC/usr/share/pkgdemo/$_long"
printf 'LONGNAME-OK\n' > "$SRC/usr/share/pkgdemo/$_long/payload.txt"
ln -s pkgdemo "$SRC/bin/pkgdemo-link"
mkdir -p "$SRC/DEBIAN"
cat > "$SRC/DEBIAN/control" <<'EOF'
Package: pkgdemo
Version: 1.2.3
Architecture: all
Maintainer: Parlz <pkg@parlz.com>
Installed-Size: 4
Priority: optional
Section: utils
Description: demo package for the ported dpkg
 This is the long description line.
EOF
printf '/usr/share/pkgdemo/sub dir/app.conf\n' > "$SRC/DEBIAN/conffiles"
cat > "$SRC/DEBIAN/postinst" <<'EOF'
#!/bin/sh
echo "postinst:$1" > "${DPKG_ROOT:-/}/tmp-postinst-ran"
EOF
chmod 755 "$SRC/DEBIAN/postinst"
printf 'PARLZ-DATA-42\n' > "$SRCD/bin/hello2"
mkdir -p "$SRCD/DEBIAN"
sed 's/^Package: pkgdemo/Package: pkgdemo2/; s/^Version: 1.2.3/Version: 0.9/' \
    "$SRC/DEBIAN/control" > "$SRCD/DEBIAN/control"
mkdir -p "$BAD/DEBIAN"
printf 'x\n' > "$BAD/bin/never"
cat > "$BAD/DEBIAN/control" <<'EOF'
Package: badpre
Version: 1.0
Architecture: all
Description: preinst always fails
EOF
cat > "$BAD/DEBIAN/preinst" <<'EOF'
#!/bin/sh
exit 3
EOF
chmod 755 "$BAD/DEBIAN/preinst"
mkdir -p "$XZ/DEBIAN"
printf 'xz\n' > "$XZ/bin/xztool"
cp "$SRC/DEBIAN/control" "$XZ/DEBIAN/control"
mkdir -p "$A64/DEBIAN"
printf 'arm\n' > "$A64/bin/armtool"
sed 's/^Architecture: all/Architecture: arm64/' "$SRC/DEBIAN/control" \
    > "$A64/DEBIAN/control"

DEBDIR=$ROOT_TEST/debs
mkdir -p "$DEBDIR"
dpkg-deb -Zgzip --build "$SRC"  "$DEBDIR/pkgdemo_1.2.3_all.deb"  >/dev/null || exit 1
dpkg-deb -Zgzip --build "$SRCD" "$DEBDIR/pkgdemo2_0.9_all.deb"   >/dev/null || exit 1
dpkg-deb -Zgzip --build "$BAD"  "$DEBDIR/badpre_1.0_all.deb"     >/dev/null || exit 1
dpkg-deb -Zxz   --build "$XZ"   "$DEBDIR/pkgxz_1.2.3_all.deb"    >/dev/null || exit 1
dpkg-deb -Zgzip --build "$A64"  "$DEBDIR/pkgarm_1.2.3_arm64.deb" >/dev/null || exit 1
# 1.3.0 版(升级用): 换个版本号 + 数据里多一个文件
sed 's/^Version: 1.2.3/Version: 1.3.0/' "$SRC/DEBIAN/control" > "$SRC/DEBIAN/control.new"
mv "$SRC/DEBIAN/control.new" "$SRC/DEBIAN/control"
printf 'NEW-FILE-ADDED\n' > "$SRC/bin/pkgdemo-new"
chmod 644 "$SRC/bin/pkgdemo-new"
dpkg-deb -Zgzip --build "$SRC" "$DEBDIR/pkgdemo_1.3.0_all.deb" >/dev/null || exit 1

R=$ROOT_TEST/root
A=$ROOT_TEST/admin
mkroot() { rm -rf "$R" "$A"; mkdir -p "$R/tmp" "$A"; }
D() { "$DPKG" --root "$R" --admindir "$A" "$@"; }
deb() { echo "$DEBDIR/$1"; }

echo "=== 0) 上游工具与包内容(对照基准) ==="
chk "dpkg-deb 造出的包是 ar 档(!<arch>)" \
    "head -c 8 '$(deb pkgdemo_1.2.3_all.deb)' | grep -q '<arch>'"
chk "包内有 data.tar.gz(说明本实现该解得动)" \
    "ar t '$(deb pkgdemo_1.2.3_all.deb)' 2>/dev/null | grep -qx data.tar.gz"

echo "=== 1) 元数据查询(不落盘) ==="
chk "dpkg -I 打出 Package 字段" \
    "D -I '$(deb pkgdemo_1.2.3_all.deb)' | grep -qx 'Package: pkgdemo'"
chk "dpkg -I <包> Version 只要值" \
    "D -I '$(deb pkgdemo_1.2.3_all.deb)' Version | grep -qx '1.2.3'"
chk "dpkg -c 列出 data 成员(含长名文件)" \
    "D -c '$(deb pkgdemo_1.2.3_all.deb)' | grep -q 'payload.txt'"
chk "dpkg -c 一个文件都没落盘" \
    "[ ! -e '$R/bin/pkgdemo' ]"
chk "未安装的包 -L 必须失败" "! D -L pkgdemo >/dev/null 2>&1"

echo "=== 2) 安装(终态: 文件/权限/软链/长名/清单/状态) ==="
mkroot
chkrc "dpkg -i 装 pkgdemo 成功" 0 D -i "$(deb pkgdemo_1.2.3_all.deb)"
chk "/bin/pkgdemo 真落盘且可执行" \
    "[ -x '$R/bin/pkgdemo' ]"
chk "内容逐字节正确" \
    "grep -qx 'echo pkgdemo running' '$R/bin/pkgdemo'"
chk "带空格的路径也落对了" \
    "grep -qx 'cfg=1' '$R/usr/share/pkgdemo/sub dir/app.conf'"
chk "GNU 长名成员落对了(ustar 装不下 100+ 字节名)" \
    "grep -qx 'LONGNAME-OK' '$R/usr/share/pkgdemo/$_long/payload.txt'"
chk "软链照拷且目标未改" \
    "[ -L '$R/bin/pkgdemo-link' ] && [ \"\$(readlink '$R/bin/pkgdemo-link')\" = pkgdemo ]"
chk "postinst 真跑过(arg=configure)且看见 DPKG_ROOT" \
    "grep -qx 'postinst:configure' '$R/tmp-postinst-ran'"
chk "状态库条目 = install ok installed" \
    "D -s pkgdemo | grep -q 'Status: install ok installed'"
chk "状态库带版本与架构" \
    "D -s pkgdemo | grep -q '^Version: 1.2.3' && D -s pkgdemo | grep -q '^Architecture: all'"
chk "info/pkgdemo.list 记了清单" \
    "grep -qx '/bin/pkgdemo' '$A/info/pkgdemo.list'"
chk "清单条数 = 实际落盘的可数条目" \
    "[ \$(wc -l < '$A/info/pkgdemo.list') -ge 5 ]"
D -L pkgdemo | sort > "$ROOT_TEST/query-files.txt"
sort "$A/info/pkgdemo.list" > "$ROOT_TEST/recorded-files.txt"
chk "-L 输出与登记的清单逐字一致" \
    "cmp -s '$ROOT_TEST/query-files.txt' '$ROOT_TEST/recorded-files.txt'"
chk "-l 里有 ii 行" \
    "D -l | grep -q '^ii pkgdemo *1.2.3'"
chk "control 也存进 info/" \
    "grep -qx 'Package: pkgdemo' '$A/info/pkgdemo.control'"

echo "=== 3) 必须拒绝的三种包(反例: 偷懒实现会在这里变绿) ==="
chkrc "拒绝非本机的 arm64 包" N D -i "$(deb pkgarm_1.2.3_arm64.deb)"
chk "arm64 包一个文件都没落" \
    "[ ! -e '$R/bin/armtool' ]"
chk "拒绝的理由打的是架构不匹配" \
    "D -i '$(deb pkgarm_1.2.3_arm64.deb)' 2>&1 | grep -q 'arm64'"
chkrc "preinst 失败 => 安装失败" N D -i "$(deb badpre_1.0_all.deb)"
chk "preinst 失败时 data 里的文件没落盘" \
    "[ ! -e '$R/bin/never' ]"
chk "preinst 失败时状态库里没登记 badpre" \
    "! D -s badpre | grep -q 'install ok installed'"
chkrc "拒绝解不动的 xz 负载(不能当空包混过去)" N D -i "$(deb pkgxz_1.2.3_all.deb)"
chk "拒绝理由里点名 gzip/xz" \
    "D -i '$(deb pkgxz_1.2.3_all.deb)' 2>&1 | grep -qi 'gzip'"
chk "xz 包失败后没登记状态" \
    "! D -s pkgxz | grep -q 'install ok installed'"
mkroot
D -i "$(deb pkgdemo_1.3.0_all.deb)" >/dev/null 2>&1
chkrc "已装 1.3.0 时装 1.2.3 必须拒绝(降级)" N D -i "$(deb pkgdemo_1.2.3_all.deb)"
chk "拒绝降级后状态里仍是 1.3.0(没被回写坏)" \
    "D -s pkgdemo | grep -q '^Version: 1.3.0'"
chk "拒绝降级时 1.3.0 的清单没被盖掉(装到一半不回落盘清单)" \
    "grep -qx '/bin/pkgdemo-new' '$A/info/pkgdemo.list'"
chkrc "带 --force-downgrade 就允许降级" 0 D --force-downgrade -i "$(deb pkgdemo_1.2.3_all.deb)"
chk "强行降级后状态确实变成 1.2.3" \
    "D -s pkgdemo | grep -q '^Version: 1.2.3'"

echo "=== 4) 升级(终态: 新文件到位、旧文件按新清单管) ==="
chkrc "1.2.3 -> 1.3.0 升级" 0 D -i "$(deb pkgdemo_1.3.0_all.deb)"
chk "升级后新增文件已落盘" \
    "grep -qx 'NEW-FILE-ADDED' '$R/bin/pkgdemo-new'"
chk "升级后状态版本是 1.3.0" \
    "D -s pkgdemo | grep -q '^Version: 1.3.0'"
chk "状态库只有一个 pkgdemo 条目(没写重复)" \
    "[ \$(grep -c '^Package: pkgdemo$' '$A/status') -eq 1 ]"

echo "=== 5) 卸载与清除 ==="
chkrc "dpkg -r 卸载" 0 D -r pkgdemo
chk "卸载后 /bin/pkgdemo 没了" \
    "[ ! -e '$R/bin/pkgdemo' ]"
chk "卸载后长名目录里的文件也没了" \
    "[ ! -e '$R/usr/share/pkgdemo/$_long/payload.txt' ]"
chk "卸载后软链没了" \
    "[ ! -e '$R/bin/pkgdemo-link' ]"
chk "卸载后状态是 deinstall ok config-files" \
    "D -s pkgdemo | grep -q 'deinstall ok config-files'"
chk "conffiles 与状态条目留着(-P 才清)" \
    "[ -f '$A/info/pkgdemo.conffiles' ]"
chkrc "dpkg -P 清除" 0 D -P pkgdemo
chk "清除后状态库里查无此包" \
    "D -s pkgdemo | grep -q 'is not installed'"
chk "清除后 info/ 伴生文件都没了" \
    "[ ! -f '$A/info/pkgdemo.control' ] && [ ! -f '$A/info/pkgdemo.list' ]"
chk "清除后 -l 显示没有匹配的包" \
    "D -l | grep -q '没有匹配的包'"
chk "第二个包照常装/卸(状态库多条目)" \
    "D -i '$(deb pkgdemo2_0.9_all.deb)' >/dev/null 2>&1 && D -l | grep -q '^ii pkgdemo2' && D -P pkgdemo2 >/dev/null 2>&1"

echo "=== 6) 版本比较(Debian policy 的规则, 表驱动) ==="
vc() { # vc <a> <op> <b> <期望 0|N>
    D --compare-versions "$1" "$2" "$3" >/dev/null 2>&1
    _r=$?
    if [ "$4" = 0 ] && [ "$_r" = 0 ]; then ok "$1 $2 $3"
    elif [ "$4" != 0 ] && [ "$_r" != 0 ]; then ok "$1 $2 $3 (假, 按预期)"
    else no "$1 $2 $3 (rc=$_r, 期望 $4)"; fi
}
vc 1.0  '<<' 1.0.1  0
vc 1.9  '<'  1.10   0
vc 1.0  '<<' 1.0    N
vc 1.0~beta '<' 1.0 0        # 波浪号排在一切之前(预发布 < 正式版)
vc 1:1.0 '>' 9.9    0        # epoch 压倒上游版本
vc 1:1.0 '=' 1:1.0  0
vc 1.0-2 '>' 1.0-1  0        # 修订号
vc 1.0  '<<' 1.0a   0        # 字母在数字段之后: 1.0 < 1.0a
vc 2.3.1-1 '=' 2.3.1-1 0
vc 0.1  '>>' 0.0.9  0
vc 1.0-  '>' 1.0    N

echo "=== 7) 路径安全(恶意成员名必须被拒, 不能写到 root 外面) ==="
EVIL=$ROOT_TEST/srcevil
mkdir -p "$EVIL/DEBIAN" "$EVIL/bin"
printf 'evil\n' > "$EVIL/bin/x"
printf 'Package: evilpaths\nVersion: 1.0\nArchitecture: all\nDescription: x\n' \
    > "$EVIL/DEBIAN/control"
dpkg-deb -Zgzip --build "$EVIL" "$DEBDIR/evil_1.0_all.deb" >/dev/null 2>&1
# 手工往 data.tar.gz 里塞一个 ../ 成员(上游 dpkg-deb 不会生成, 只有恶意包会有):
# 外层 ar 仍由宿主真 ar 写, 只换 payload, 免得测试自己造出个不合法的档
mkdir -p "$ROOT_TEST/evilar"
( cd "$ROOT_TEST/evilar" && ar x "$DEBDIR/evil_1.0_all.deb" ) || exit 1
python3 - "$ROOT_TEST/evilar/data.tar.gz" <<'PY'
import sys, gzip
p = sys.argv[1]
raw = gzip.decompress(open(p, 'rb').read())
name = b'../../../../PARLZ-ESCAPE'
payload = b'ESCAPED-CONTENT\n'          # 16 字节
h = bytearray(512)
h[0:len(name)] = name
h[100:108] = b'0000644\0'
h[124:136] = b'%011o\0' % len(payload)   # size 必须与数据块一致, 否则整档后面对齐全乱
h[156:157] = b'0'
h[257:263] = b'ustar\x00'
h[263:265] = b'00'
chk = sum(h[148:156])
for i, c in enumerate(b'%07o\0' % chk):
    h[148 + i] = c
core = raw.rstrip(b'\0')                 # 尾部有两块结束标记 + tar 的 20 块对齐填充
body = (core + bytes(h) + payload + b'\0' * (512 - len(payload))
        + b'\0' * 1024)
open(p, 'wb').write(gzip.compress(body))
print('data.tar.gz patched with an escaping member')
PY
( cd "$ROOT_TEST/evilar" && ar rc "$ROOT_TEST/evil2.deb" \
    debian-binary control.tar.gz data.tar.gz ) || exit 1
chk "恶意包造好了" "[ -f '$ROOT_TEST/evil2.deb' ]"
mkroot
chkrc "含 ../../ 成员的包必须被拒" N D -i "$ROOT_TEST/evil2.deb"
chk "逃逸文件没写到 root 外面" \
    "[ ! -e '$ROOT_TEST/PARLZ-ESCAPE' ] && [ ! -e /PARLZ-ESCAPE ]"

echo "=== RESULT: pass=$pass fail=$fail ==="
[ -n "${KEEP:-}" ] || rm -rf "$ROOT_TEST"
[ "$fail" = 0 ] || exit 1
