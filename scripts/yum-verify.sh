#!/bin/sh
# yum-verify.sh - 自研 yum 前端的宿主侧回归(秒级, 不起 QEMU)。
#
# 仓库是**真仓库**: .rpm 由宿主 rpmbuild 产出, repodata 由宿主 createrepo_c
# 生成(repomd.xml / primary.xml.gz / sha256 摘要都是上游工具写的), 再用
# python3 起静态 http 站点当镜像。yum 走自己的 repomd→primary 解析、依赖求解、
# 下载校验, 最后 fork+execv 调本仓库的 rpm 落盘。
#
# 判据全是终态: 依赖闭包里的包全都装上、虚拟/文件型依赖真被解析、摘要不符时
# 一个文件都不落、gpgcheck=1 的源不装、升级后旧条目消失、卸载后文件真没了。
#
#   sh scripts/yum-verify.sh [yum 二进制] [rpm 二进制]
set -u
T=/tmp/parlz-yum-test
YUM=${1:-/mnt/f/Linux/Parlz/userland/build/bin/yum}
RPM=${2:-/mnt/f/Linux/Parlz/userland/build/bin/rpm}
PORT=${PORT:-18772}
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

for f in "$YUM" "$RPM"; do
    [ -x "$f" ] || { echo "!! 缺可执行文件: $f"; exit 1; }
done
for t in rpmbuild createrepo_c python3; do
    command -v $t >/dev/null 2>&1 || { echo "!! 宿主缺 $t —— 造不出真 repodata"; exit 1; }
done

rm -rf "$T"
mkdir -p "$T/SPECS" "$T/RPMS" "$T/repo"
# mk <包名> <版本> <release> <额外头部行> <额外%files行> <额外%pre体>
mk() {
    cat > "$T/SPECS/$1-$2-$3.spec" <<EOS
Name: $1
Version: $2
Release: $3
Summary: $1 fixture for the ported yum
License: GPL-2.0
BuildArch: x86_64
$4
%description
$1 fixture package.
%pre
echo "PRE \$1" >> "\$RPM_ROOT/log-$1-pre"
$6
%post
echo "POST \$1" >> "\$RPM_ROOT/log-$1-post"
%install
mkdir -p %{buildroot}/usr/local/bin
printf '#!/bin/sh\necho $1-$2-$3\n' > %{buildroot}/usr/local/bin/$1
chmod 755 %{buildroot}/usr/local/bin/$1
%files
%defattr(-,root,root,-)
/usr/local/bin/$1
$5
EOS
}
mk yumbase  1.0 1 "" "" ""
mk yumbase  1.0 2 "" "" ""                     # 同包名两个 release: 选版本要用
mk yumdep   1.0 1 "Requires: yumbase >= 1.0" "" ""
mk yumfil   1.0 1 "Requires: /usr/local/bin/yumbase" "" ""   # 文件型依赖
mk yumprov  1.0 1 "Provides: virtual-thing = 1.0" "" ""
mk yumvirt  1.0 1 "Requires: virtual-thing" "" ""            # 虚拟型依赖
mk yumorph  1.0 1 "Requires: nothing-provides-this" "" ""    # 满足不了
mk yumbad   1.0 1 "" "" "exit 3"                            # %pre 必失败
mk yumsolo  1.0 1 "" "" ""

for s in "$T"/SPECS/*.spec; do
    rpmbuild --define "_topdir $T" --define "debug_package %{nil}" \
             --define "_rpmdir $T/RPMS" -bb "$s" \
             >"$T/build-$(basename "$s").log" 2>&1 \
    || { echo "!! rpmbuild $s 失败"; tail -6 "$T/build-$(basename "$s").log"; exit 1; }
done
find "$T/RPMS" -name '*.rpm' -exec cp {} "$T/repo/" \;
createrepo_c "$T/repo" >"$T/createrepo.log" 2>&1 \
    || { echo "!! createrepo_c 失败"; tail -10 "$T/createrepo.log"; exit 1; }
[ -f "$T/repo/repodata/repomd.xml" ] || { echo "!! 没有 repomd.xml"; exit 1; }
echo "  fixture: $(ls "$T/repo"/*.rpm | wc -l) 个 .rpm 进了仓库"

python3 -m http.server $PORT --directory "$T/repo" --bind 127.0.0.1 \
    >"$T/httpd.log" 2>&1 &
HTTPD=$!
trap 'kill $HTTPD 2>/dev/null' EXIT INT TERM
i=0
while ! python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(1)
sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)" 2>/dev/null; do
    i=$((i+1))
    if [ $i -gt 40 ]; then echo "!! 站点起不来"; cat "$T/httpd.log"; exit 1; fi
    sleep 0.25
done
ok "本地仓库站点在 $PORT 上应答"

R=$T/root; DB=$R/var/lib/rpm/installed
mkroot() { rm -rf "$R"; mkdir -p "$R/usr/local/bin" "$R/bin" \
                  "$R/etc/yum.repos.d" "$R/var/lib/rpm" "$R/var/cache/yum"
           # 交付根里本来就有 /bin/sh —— 脚本包自动生成的 "Requires: /bin/sh"
           # 靠它满足(这正是"文件型依赖按盘上有没有来判断"那条路)
           printf '#!/bin/sh\nexec /bin/busybox sh "$@"\n' > "$R/bin/sh"
           chmod 755 "$R/bin/sh"
           printf '[parlz-local]\nname=Parlz local repo\nbaseurl=http://127.0.0.1:%s\ngpgcheck=0\nenabled=1\n' \
               "$PORT" > "$R/etc/yum.repos.d/parlz.repo"; }
Y() { "$YUM" --installroot "$R" --rpm "$RPM" "$@"; }
Q() { "$RPM" --root "$R" "$@"; }
NDB() { ls "$DB" 2>/dev/null | grep -c '\.meta$'; }

echo "=== 0) fixture: createrepo_c 生成的真 repodata ==="
chk "repodata 里有 primary.xml.gz" \
    "ls '$T/repo/repodata' | grep -q 'primary.xml.gz'"
chk "repomd.xml 的 primary 条目带 sha256" \
    "grep -A3 'type=\"primary\"' '$T/repo/repodata/repomd.xml' | grep -q 'type=\"sha256\"'"
chk "primary.xml 里确有 virtual-thing 与那个文件路径" \
    "python3 -c \"
import gzip,glob,sys
d=gzip.open(glob.glob('$T/repo/repodata/*primary.xml.gz')[0]).read().decode('utf8','replace')
sys.exit(0 if ('virtual-thing' in d and '/usr/local/bin/yumbase' in d) else 1)\""

echo "=== 1) makecache 与只查不动盘 ==="
mkroot
run "yum makecache" 0 Y makecache
chk "索引缓存落到 <cache>/parlz-local/primary.xml" \
    "[ -f '$R/var/cache/yum/parlz-local/primary.xml' ]"
chk "缓存没被写成双层目录(<cache>/<id>/<id>/)" \
    "[ ! -e '$R/var/cache/yum/parlz-local/parlz-local' ]"
chk "yum list 打出 yumdep" "Y list | grep -q 'yumdep'"
chk "yum info 打出 Name/Size/Repo" \
    "Y info yumbase | grep -q '^Name    : yumbase' && Y info yumbase | grep -q '^Size    :' && Y info yumbase | grep -q '^Repo    : parlz-local'"
chk "yum search fixture 命中多条" "[ \$(Y search fixture | grep -c '^yum') -ge 6 ]"
chk "yum repolist 打出 baseurl" "Y repolist | grep -q \"127.0.0.1:$PORT\""
chk "只查询没装任何东西" \
    "[ ! -e '$R/usr/local/bin/yumbase' ] && [ ! -e '$R/usr/local/bin/yumdep' ]"

echo "=== 2) 版本选择: 同包名两个 release 要拿高的 ==="
chk "候选是 release 2 而不是 1" "Y info yumbase | grep -q '^Release : 2'"

echo "=== 3) 包依赖闭包 ==="
run "yum install yumdep" 0 Y -y install yumdep
chk "被依赖的 yumbase 装了, 且装的是 release 2" \
    "[ -x '$R/usr/local/bin/yumbase' ] && grep -qx 'echo yumbase-1.0-2' '$R/usr/local/bin/yumbase'"
chk "要的 yumdep 装了" "[ -x '$R/usr/local/bin/yumdep' ]"
chk "rpm 库里两条都在" \
    "Q -q yumbase | grep -qx 'yumbase-1.0-2.x86_64' && Q -q yumdep | grep -qx 'yumdep-1.0-1.x86_64'"
chk "包自己的 %pre/%post 真跑过" \
    "grep -q 'PRE 1' '$R/log-yumbase-pre' && grep -q 'POST 1' '$R/log-yumbase-post'"
chk "yum list installed 两条都在" \
    "Y list installed | grep -q yumbase && Y list installed | grep -q yumdep"

echo "=== 4) 文件型与虚拟型依赖 ==="
mkroot
Y makecache >/dev/null 2>&1
run "yum install yumfil(Requires 一个文件路径)" 0 Y -y install yumfil
chk "它把提供该文件的 yumbase 一起拉进来了" \
    "[ -x '$R/usr/local/bin/yumfil' ] && [ -x '$R/usr/local/bin/yumbase' ]"
mkroot
Y makecache >/dev/null 2>&1
run "yum install yumvirt(Requires 虚拟名)" 0 Y -y install yumvirt
chk "它把 Provides 这个虚拟名的 yumprov 拉进来了" \
    "[ -x '$R/usr/local/bin/yumvirt' ] && [ -x '$R/usr/local/bin/yumprov' ]"

echo "=== 5) 依赖满足不了: 整体失败, 什么都没落 ==="
mkroot
Y makecache >/dev/null 2>&1
run "yum install yumorph 要失败" N Y -y install yumorph
chk "yumorph 没落盘" "[ ! -e '$R/usr/local/bin/yumorph' ]"
chk "库里也没登记" "! Q -q yumorph >/dev/null 2>&1"
chk "错误里点名缺的那个能力" \
    "Y -y install yumorph 2>&1 | grep -q 'nothing-provides-this'"
run "--skip-broken 返回成功(它是'丢掉这个请求')" 0 \
    Y -y --skip-broken install yumorph
chk "--skip-broken 之后 yumorph 仍然没装(不是硬装一个跑不起来的包)" \
    "[ ! -e '$R/usr/local/bin/yumorph' ] && ! Q -q yumorph >/dev/null 2>&1"
run "仓库里没有的包名要失败" N Y -y install nosuchpkg-xyz

echo "=== 6) rpm 侧失败(%pre 退出 3)要干净 ==="
mkroot
Y makecache >/dev/null 2>&1
N0=$(NDB)
run "yum install yumbad 要失败" N Y -y install yumbad
chk "yumbad 的文件没落盘" "[ ! -e '$R/usr/local/bin/yumbad' ]"
chk "库里条目数没变($N0)" "[ \$(NDB) -eq $N0 ]"

echo "=== 7) 摘要闸门: 包体与索引都不许被偷偷换过 ==="
mkroot
Y makecache >/dev/null 2>&1
cp "$T/repo/yumsolo-1.0-1.x86_64.rpm" "$T/yumsolo.orig.rpm"
python3 - "$T/repo/yumsolo-1.0-1.x86_64.rpm" <<'PY'
import sys
p = sys.argv[1]
b = bytearray(open(p, "rb").read())
i = len(b) - 40
b[i] ^= 0xFF                      # 等长改一个字节: Size 对得上, 只能靠 sha256
open(p, "wb").write(bytes(b))
print("flipped one byte (size unchanged)")
PY
run "内容被改过的包必须被拒" N Y -y install yumsolo
chk "改过的包没落盘" "[ ! -e '$R/usr/local/bin/yumsolo' ]"
chk "拒绝理由点名 sha256 不符" \
    "Y -y install yumsolo 2>&1 | grep -qi 'sha256'"
chk "坏包没留在缓存里" \
    "! find '$R/var/cache/yum' -name 'yumsolo*.rpm' | grep -q ."
# 把 yumsolo 修回原样: 后面几节还要用它(而且"修好就能装"本身就是对照)
cp "$T/yumsolo.orig.rpm" "$T/repo/yumsolo-1.0-1.x86_64.rpm"
run "包修回原样后同一个包就能装了(证明上一关不是恒失败)" 0 \
    Y -y install yumsolo
chk "修好后文件真在" "[ -x '$R/usr/local/bin/yumsolo' ]"
Q -e yumsolo >/dev/null 2>&1
cp "$T/repo/repodata/repomd.xml" "$T/repomd.bak"
python3 - "$T/repo/repodata/repomd.xml" <<'PY'
import sys, re
p = sys.argv[1]
s = open(p, encoding="utf-8").read()
m = re.search(r'<data type="primary">.*?<checksum type="sha256">([0-9a-f]{64})',
              s, re.S)
if not m:
    raise SystemExit("没找到 primary 的 sha256")
old = m.group(1)
new = ("0" if old[0] != "0" else "1") + old[1:]
s = s[:m.start(1)] + new + s[m.end(1):]
open(p, "w", encoding="utf-8", newline="\n").write(s)
print("repomd 里 primary 的摘要被改成 %s" % new)
PY
run "repomd 摘要不符时 makecache 要失败" N Y makecache
chk "失败理由点名索引摘要不符" \
    "Y makecache 2>&1 | grep -q '索引摘要不符'"
cp "$T/repomd.bak" "$T/repo/repodata/repomd.xml"
run "修回原样之后 makecache 又能跑" 0 Y makecache

echo "=== 8) 信任闸门: gpgcheck=1 不装 ==="
mkroot
printf '[parlz-local]\nname=s\nbaseurl=http://127.0.0.1:%s\ngpgcheck=1\nenabled=1\n' "$PORT" \
    > "$R/etc/yum.repos.d/parlz.repo"
run "gpgcheck=1 时 makecache 能跑" 0 Y makecache
run "gpgcheck=1 时 install 必须被拒" N Y -y install yumsolo
chk "拒绝理由点名 gpgcheck" \
    "Y -y install yumsolo 2>&1 | grep -qi 'gpgcheck'"
chk "被拒的包没落盘" "[ ! -e '$R/usr/local/bin/yumsolo' ]"
run "--nogpgcheck 明示放行才装" 0 Y --nogpgcheck -y install yumsolo
chk "放行后装上了" "[ -x '$R/usr/local/bin/yumsolo' ]"

echo "=== 9) 升级与卸载 ==="
mkroot
Y makecache >/dev/null 2>&1
RPM1=$(find "$T/RPMS" -name 'yumbase-1.0-1.*.rpm' | head -1)
"$RPM" --root "$R" -i "$RPM1" >/dev/null 2>&1
chk "前置: 库里是 yumbase-1.0-1" \
    "Q -q yumbase | grep -qx 'yumbase-1.0-1.x86_64'"
run "yum update yumbase 升到 release 2" 0 Y -y update yumbase
chk "升级后库里只有 1.0-2" \
    "Q -qa | grep -q 'yumbase-1.0-2' && ! Q -qa | grep -q 'yumbase-1.0-1'"
run "yum remove yumbase" 0 Y -y remove yumbase
chk "文件真没了" "[ ! -e '$R/usr/local/bin/yumbase' ]"
chk "库里没有了" "! Q -q yumbase >/dev/null 2>&1"
run "删没装过的包要失败" N Y -y remove nosuchpkg-xyz
run "yum clean all" 0 Y clean all
chk "缓存里没有 primary.xml 了" \
    "! find '$R/var/cache/yum' -name primary.xml | grep -q ."

echo "=== 10) 仓库不可达 / 没配置 / 全禁用 ==="
mkroot
rm -f "$R/etc/yum.repos.d"/*.repo            # mkroot 自带一个能用的, 这几例要清场
printf '[dead]\nname=dead\nbaseurl=http://127.0.0.1:1\ngpgcheck=0\nenabled=1\n' \
    > "$R/etc/yum.repos.d/dead.repo"
run "仓库不可达时 makecache 返回非 0" N Y makecache
run "仓库不可达时 install 也返回非 0" N Y -y install yumbase
chk "失败理由点名连接失败" \
    "Y makecache 2>&1 | grep -q '失败'"
rm -f "$R/etc/yum.repos.d/dead.repo"
run "没有任何 .repo 时 makecache 报错" N Y makecache
chk "报错里点出 yum.repos.d 路径" \
    "Y makecache 2>&1 | grep -q 'yum.repos.d'"
printf '[off]\nname=off\nbaseurl=http://127.0.0.1:%s\ngpgcheck=0\nenabled=0\n' "$PORT" \
    > "$R/etc/yum.repos.d/off.repo"
run "只有 enabled=0 的仓库时 makecache 失败" N Y makecache
chk "禁用仓库不产缓存" \
    "[ ! -f '$R/var/cache/yum/off/primary.xml' ]"
run "--enablerepo 点名才生效" 0 Y --enablerepo=off makecache
chk "--enablerepo=off 之后真出了缓存" \
    "[ -f '$R/var/cache/yum/off/primary.xml' ]"
printf 'baseurl=http://127.0.0.1:%s\nname=no-section\n' "$PORT" \
    > "$R/etc/yum.repos.d/broken.repo"
chk "键写在 [段] 之前要点名(不能静默忽略)" \
    "Y makecache 2>&1 | grep -q '段之前'"

echo "=== RESULT: pass=$pass fail=$fail ==="
kill $HTTPD 2>/dev/null
[ "$fail" = 0 ] || exit 1
[ -n "${KEEP:-}" ] || rm -rf "$T"
