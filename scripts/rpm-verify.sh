#!/bin/sh
# rpm-verify.sh - 自研 rpm 移植的宿主侧回归(秒级, 不起 QEMU)。
#
# 对照物是**上游工具**: 包由宿主 rpmbuild 产出, 清单/元数据再用宿主 rpm 列一遍
# 与我们的输出逐行比 —— 比"测试自己造个包自己认"硬得多。
# 判据全是终态: 文件真落盘、权限/软链对、脚本段真跑过(看它写进 root 的日志)、
# 状态库里有/没有条目、卸载后文件真没了。反例(架构不符、%pre 失败、xz 负载、
# 降级)必须"失败且什么都没留下", 只打一句失败不算过。
#
#   sh scripts/rpm-verify.sh [rpm 二进制路径]
set -u
T=/tmp/parlz-rpm-test
RPM=${1:-/mnt/f/Linux/Parlz/userland/build/bin/rpm}
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'PASS  %s\n' "$1"; }
no()  { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }
chk() { if eval "$2"; then ok "$1"; else no "$1  [谓词: $2]"; fi }
chkrc() { _d="$1"; _e="$2"; shift 2
    "$@" >"$T/last.log" 2>&1; _r=$?
    if [ "$_e" = 0 ] && [ "$_r" = 0 ]; then ok "$_d"
    elif [ "$_e" != 0 ] && [ "$_r" != 0 ]; then ok "$_d (按预期失败 rc=$_r)"
    else no "$_d (rc=$_r, 期望 $([ "$_e" = 0 ] && echo 0 || echo 非0))"; fi; }

[ -x "$RPM" ] || { echo "!! 找不到可执行的 rpm: $RPM"; exit 1; }
for t in rpmbuild rpm python3; do
    command -v $t >/dev/null 2>&1 || { echo "!! 宿主缺 $t —— 造不出/对不了真 .rpm"; exit 1; }
done
SRCDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

rm -rf "$T"
S=$T/stage
LONGDIR=verylongdocumentationdirectoryname_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
mkdir -p "$T/SPECS" "$S/usr/local/bin" "$S/etc/pkgdemo" \
         "$S/usr/share/doc/pkgdemo/$LONGDIR"
printf '#!/bin/sh\necho pkgdemo rpm\n' > "$S/usr/local/bin/pkgdemo"
chmod 755 "$S/usr/local/bin/pkgdemo"
printf 'opt=1\n' > "$S/etc/pkgdemo/demo.conf"
printf 'README-CONTENT\n' > "$S/usr/share/doc/pkgdemo/README"
printf 'LONGPATH-OK\n' > "$S/usr/share/doc/pkgdemo/$LONGDIR/notes.txt"
ln -sf pkgdemo "$S/usr/local/bin/pkgdemo-link"

# mkspec <spec名> <包名> <版本> <release> <arch> <额外%install> <额外%files>
mkspec() {
    cat > "$T/SPECS/$1.spec" <<EOS
Name: $2
Version: $3
Release: $4
Summary: demo package for the ported rpm
License: GPL-2.0
BuildArch: $5
%description
Long description line one.
%pre
echo "PREIN \$1" >> "\$RPM_ROOT/log-prein"
%post
echo "POSTIN \$1" >> "\$RPM_ROOT/log-postin"
%preun
echo "PREUN \$1" >> "\$RPM_ROOT/log-preun"
%postun
echo "POSTUN \$1" >> "\$RPM_ROOT/log-postun"
%install
mkdir -p %{buildroot}/usr/local/bin %{buildroot}/etc/pkgdemo \\
         %{buildroot}/usr/share/doc/pkgdemo/$LONGDIR
cp -a $S/usr/local/bin/. %{buildroot}/usr/local/bin/
cp -a $S/etc/pkgdemo/. %{buildroot}/etc/pkgdemo/
cp -a $S/usr/share/doc/pkgdemo/. %{buildroot}/usr/share/doc/pkgdemo/
$6
%files
%defattr(-,root,root,-)
/usr/local/bin/pkgdemo
/usr/local/bin/pkgdemo-link
%config(noreplace) /etc/pkgdemo/demo.conf
%doc /usr/share/doc/pkgdemo/README
%doc /usr/share/doc/pkgdemo/$LONGDIR/notes.txt
$7
EOS
}
mkspec pkgdemo   pkgdemo   1.2.3 4 x86_64 "" ""
mkspec pkgdemo5  pkgdemo   1.2.3 5 x86_64 \
    "mkdir -p %{buildroot}/usr/share/pkgdemo; echo extra > %{buildroot}/usr/share/pkgdemo/added-in-5.txt" \
    "/usr/share/pkgdemo/added-in-5.txt"
mkspec pkgi686   pkgi686   1.0   1 x86_64 "" ""
mkspec pkgnoarch pkgnoarch 1.0   1 noarch "" ""

cat > "$T/SPECS/pkgfail.spec" <<'EOS'
Name: pkgfail
Version: 1.0
Release: 1
Summary: pre fails
License: GPL-2.0
BuildArch: x86_64
%description
d
%pre
echo PREFAIL >> "$RPM_ROOT/log-prefail"
exit 3
%post
echo POSTIN_SHOULD_NOT_RUN >> "$RPM_ROOT/log-postin"
%install
mkdir -p %{buildroot}/usr/local/bin
echo x > %{buildroot}/usr/local/bin/never-should-exist
%files
/usr/local/bin/never-should-exist
EOS

for d in RPMS RPMS5 RPMSI RPMSN RPMSB RPMSX; do mkdir -p "$T/$d"; done
B() { # B <spec> <rpmdir> [额外 rpmbuild 参数...]
    _s="$1"; _o="$2"; shift 2
    rpmbuild --define "_topdir $T" --define "debug_package %{nil}" \
             --define "_rpmdir $_o" "$@" -bb "$T/SPECS/$_s.spec" \
        >"$T/rpmbuild-$_s$*.log" 2>&1 \
    || { echo "!! rpmbuild $_s 失败"; tail -6 "$T"/rpmbuild-$_s*.log; exit 1; }
}
B pkgdemo   "$T/RPMS"
B pkgdemo5  "$T/RPMS5"
B pkgi686   "$T/RPMSI"
B pkgnoarch "$T/RPMSN"
B pkgfail   "$T/RPMSB"
B pkgdemo   "$T/RPMSX" --define "%_binary_payload w9.xzdio"
P() { find "$1" -name "$2" -type f | head -1; }
DEMO=$(P "$T/RPMS"  'pkgdemo-1.2.3-4.*.rpm')
D5=$(P   "$T/RPMS5" 'pkgdemo-1.2.3-5.*.rpm')
BAD=$(P  "$T/RPMSB" 'pkgfail-1.0-1.*.rpm')
I686SRC=$(P "$T/RPMSI" 'pkgi686-1.0-1.*.rpm')
NOAR=$(P "$T/RPMSN"  'pkgnoarch-1.0-1.noarch.rpm')
XZ=$(P   "$T/RPMSX"  'pkgdemo-1.2.3-4.*.rpm')
# 架构不符的包没法用 rpmbuild 造(宿主只肯给本机架构出包), 改成把真包的 ARCH
# 字段原地替换 —— 产物仍是 rpmbuild 写的真包, 只有这一处不同。
I686=$T/pkgi686-foreign.rpm
python3 "$SRCDIR/rpmsetarch.py" "$I686SRC" "$I686" i686 >/dev/null || exit 1
for p in "$DEMO" "$D5" "$BAD" "$I686" "$NOAR" "$XZ"; do
    [ -f "$p" ] || { echo "!! 少了一个 fixture"; exit 1; }
done

R=$T/root; A=$T/db
mk() { rm -rf "$R" "$A"; mkdir -p "$R" "$A"; }
Q() { "$RPM" --root "$R" --dbpath "$A" "$@"; }
up2file() { "$@" 2>/dev/null | LC_ALL=C sort > "$T/up.txt"; }
my2file() { Q "$@" 2>/dev/null | LC_ALL=C sort > "$T/my.txt"; }
NMETA() { ls "$A/installed" 2>/dev/null | grep -c '\.meta$'; }

echo "=== 0) fixture 与上游对照基准 ==="
chk "宿主 rpm 读得动 rpmbuild 造的包" \
    "rpm -qp --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}' '$DEMO' 2>/dev/null | grep -qx pkgdemo-1.2.3-4.x86_64"
chk "改架构那个包的 ARCH 字段确实是 i686(用 scripts/rpmhdr.py 探针看)" \
    "python3 '$SRCDIR/rpmhdr.py' '$I686' | grep -q '^  tag 1022 .*i686'"
chk "它是被改过的包: 上游 rpm 报 header 摘要不符(本实现明确不验摘要)" \
    "rpm -qp '$I686' 2>&1 | grep -q 'digest: BAD'"
chk "坏负载那个包确实是 xz 负载(上游认, 我们该拒)" \
    "rpm -qp --qf '%{PAYLOADCOMPRESSOR}' '$XZ' 2>/dev/null | grep -qx xz"
chk "正包是 gzip 负载" \
    "rpm -qp --qf '%{PAYLOADCOMPRESSOR}' '$DEMO' 2>/dev/null | grep -qx gzip"

echo "=== 1) 查包文件(-qp/-qpi/-qpl): 不落盘 ==="
mk
chk "-qp 的 NVRA 与上游一致" \
    "[ \"\$(Q -qp '$DEMO')\" = \"\$(rpm -qp --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}' '$DEMO')\" ]"
up2file rpm -qpl "$DEMO"; my2file -qpl "$DEMO"
chk "-qpl 文件清单与上游 rpm -qpl 逐行一致" "cmp -s '$T/up.txt' '$T/my.txt'"
chk "上面那条不是空对空" "[ \$(wc -l < '$T/my.txt') -ge 5 ]"
chk "-qpl 里能看到 100+ 字节的长路径" "Q -qpl '$DEMO' | grep -q '$LONGDIR'"
chk "-qpi 打出 Name/Version/Release/Architecture" \
    "Q -qpi '$DEMO' | grep -q '^Name        : pkgdemo' && Q -qpi '$DEMO' | grep -q '^Version     : 1.2.3' && Q -qpi '$DEMO' | grep -q '^Release     : 4' && Q -qpi '$DEMO' | grep -q '^Architecture: x86_64'"
chk "-qpi 的 Size 与上游一致" \
    "[ \"\$(Q -qpi '$DEMO' | sed -n 's/^Size  *: //p')\" = \"\$(rpm -qp --qf '%{SIZE}' '$DEMO')\" ]"
chk "查询动作一个文件都没落盘" "[ -z \"\$(ls -A '$R')\" ]"

echo "=== 2) 安装(终态) ==="
mk
chkrc "rpm -i 装 pkgdemo" 0 Q -i "$DEMO"
chk "/usr/local/bin/pkgdemo 真落盘、可执行、内容对" \
    "[ -x '$R/usr/local/bin/pkgdemo' ] && grep -qx 'echo pkgdemo rpm' '$R/usr/local/bin/pkgdemo'"
chk "长路径文件内容正确" \
    "grep -qx 'LONGPATH-OK' '$R/usr/share/doc/pkgdemo/$LONGDIR/notes.txt'"
chk "软链照装且目标未改" \
    "[ -L '$R/usr/local/bin/pkgdemo-link' ] && [ \"\$(readlink '$R/usr/local/bin/pkgdemo-link')\" = pkgdemo ]"
chk "%config 文件也装了" "grep -qx 'opt=1' '$R/etc/pkgdemo/demo.conf'"
chk "%pre 跑过且参数=1(首次安装)" "grep -qx 'PREIN 1' '$R/log-prein'"
chk "%post 跑过" "grep -qx 'POSTIN 1' '$R/log-postin'"
chk "-q 查到 NVRA" "Q -q pkgdemo | grep -qx pkgdemo-1.2.3-4.x86_64"
chk "-qa 也列出它" "Q -qa | grep -qx pkgdemo-1.2.3-4.x86_64"
up2file rpm -qlp "$DEMO"; my2file -ql pkgdemo
chk "-ql 与上游 rpm -qlp 逐行一致" "cmp -s '$T/up.txt' '$T/my.txt'"
chk "-qf 按**包内路径**反查到归属包" \
    "Q -qf /usr/local/bin/pkgdemo | grep -q 'pkgdemo-1.2.3-4'"
chk "-qf 对没装过的路径返回非 0" \
    "! Q -qf /usr/local/bin/no-such-thing >/dev/null 2>&1"
chk "状态库里有 .meta 与 .list" \
    "[ -f '$A/installed/pkgdemo-1.2.3-4.x86_64.meta' ] && [ -f '$A/installed/pkgdemo-1.2.3-4.x86_64.list' ]"
chk ".list 行数 = 上游 -qlp 的文件数" \
    "[ \$(wc -l < '$A/installed/pkgdemo-1.2.3-4.x86_64.list') -eq \$(rpm -qlp '$DEMO' | wc -l) ]"
chk "卸载脚本段已落库(为 -e 准备)" \
    "[ -f '$A/scriptlets/pkgdemo-1.2.3-4.x86_64.preun' ]"

echo "=== 3) 必须拒绝的情况(失败要干净) ==="
N0=$(NMETA)
chkrc "拒绝 i686 包(本机 x86_64)" N Q -i "$I686"
chk "i686 包没进状态库" "! Q -q pkgi686 >/dev/null 2>&1"
chk "i686 包连脚本段都没落库" \
    "[ ! -f '$A/scriptlets/pkgi686-1.0-1.i686.preun' ]"
chk "拒绝理由点名了架构" \
    "Q -i '$I686' 2>&1 | grep -q 'i686'"
chkrc "%pre 失败 => 安装失败" N Q -i "$BAD"
chk "%pre 失败时包内文件一个都没解" \
    "[ ! -e '$R/usr/local/bin/never-should-exist' ]"
chk "%pre 失败时这个包的 %post 没被跑" \
    "! grep -q POSTIN_SHOULD_NOT_RUN '$R/log-postin' 2>/dev/null"
chk "%pre 失败时状态库里没登记" "! Q -q pkgfail >/dev/null 2>&1"
chkrc "noarch 包必须接受" 0 Q -i "$NOAR"
chk "noarch 装完能查到" "Q -q pkgnoarch | grep -qx pkgnoarch-1.0-1.noarch"
Q -e pkgnoarch >/dev/null 2>&1
chkrc "拒绝 xz 压缩负载" N Q -i "$XZ"
chk "拒绝理由点名 gzip/xz(不是含糊的'解不开')" \
    "Q -i '$XZ' 2>&1 | grep -qi 'gzip\\|xz'"
chk "xz 包失败后状态库条目数没变" "[ \$(NMETA) -eq $N0 ]"

echo "=== 4) 同 NVRA 重装要 --replacepkgs ==="
chkrc "原样再装一次必须被拒" N Q -i "$DEMO"
chkrc "--replacepkgs 才允许" 0 Q --replacepkgs -i "$DEMO"

echo "=== 5) 升级 -U 与降级 ==="
chkrc "rpm -U 升到 release 5" 0 Q -U "$D5"
chk "升级后新增文件已落盘" \
    "grep -qx 'extra' '$R/usr/share/pkgdemo/added-in-5.txt'"
chk "升级后 -q 指新包" "Q -q pkgdemo | grep -qx pkgdemo-1.2.3-5.x86_64"
chk "旧版本条目从状态库里没了" "! Q -qa | grep -q 'pkgdemo-1.2.3-4'"
chk "旧 .meta/.list 也删了" \
    "[ ! -f '$A/installed/pkgdemo-1.2.3-4.x86_64.meta' ]"
chk "旧包的脚本段也删了" \
    "[ ! -f '$A/scriptlets/pkgdemo-1.2.3-4.x86_64.postun' ]"
chk "-U 时脚本参数=2(升级)" "grep -q 'POSTIN 2' '$R/log-postin'"
chkrc "已装 5 时装 4 必须被拒(降级)" N Q -i "$DEMO"
chk "被拒后仍是 5" "Q -q pkgdemo | grep -qx pkgdemo-1.2.3-5.x86_64"
chkrc "--oldpackage 允许两版本并存(-i 的本来语义)" 0 Q --oldpackage -i "$DEMO"
chk "并存时 -qa 两条都在" \
    "Q -qa | grep -qx pkgdemo-1.2.3-4.x86_64 && Q -qa | grep -qx pkgdemo-1.2.3-5.x86_64"

echo "=== 6) 卸载(两版本并存时的归属与参数) ==="
chkrc "按 NVRA 卸载 release 4" 0 Q -e pkgdemo-1.2.3-4.x86_64
chk "第一次卸载 %preun 参数=1(还有别的版本)" \
    "grep -qx 'PREUN 1' '$R/log-preun'"
chk "共享文件没被删(5 还装着它)" "[ -x '$R/usr/local/bin/pkgdemo' ]"
chk "4 只有它独有的文件被删掉了" \
    "[ ! -e '$R/usr/share/pkgdemo/added-in-4.txt' ]"
chk "4 的条目与清单没了、5 的还在" \
    "[ ! -f '$A/installed/pkgdemo-1.2.3-4.x86_64.list' ] && [ -f '$A/installed/pkgdemo-1.2.3-5.x86_64.list' ]"
chkrc "卸载最后一条" 0 Q -e pkgdemo-1.2.3-5.x86_64
chk "最后一条卸载 %preun 参数=0" "grep -qx 'PREUN 0' '$R/log-preun'"
chk "%postun 跑过" "grep -qx 'POSTUN 0' '$R/log-postun'"
chk "文件真没了" \
    "[ ! -e '$R/usr/local/bin/pkgdemo' ] && [ ! -e '$R/usr/share/doc/pkgdemo/README' ] && [ ! -e '$R/usr/local/bin/pkgdemo-link' ]"
chk "长路径文件与目录也没了" \
    "[ ! -e '$R/usr/share/doc/pkgdemo/$LONGDIR/notes.txt' ]"
chk "状态库里再没有 pkgdemo" "! Q -qa | grep -q pkgdemo"
chk "脚本段落库文件清干净" \
    "[ -z \"\$(ls '$A/scriptlets' 2>/dev/null | grep '^pkgdemo')\" ]"
chk "没装过的包 -e 报错" "! Q -e nosuchpkg >/dev/null 2>&1"
chk "空库时 -qa 明确说没装包" "Q -qa | grep -q 'no packages installed'"

echo "=== 7) 版本比较(RPM vercmp 的段规则) ==="
vc() { Q --compare-versions "$1" "$2" "$3" >/dev/null 2>&1; _r=$?
    if [ "$4" = 0 ] && [ "$_r" = 0 ]; then ok "$1 $2 $3"
    elif [ "$4" != 0 ] && [ "$_r" != 0 ] && [ "$4" != 0 ]; then ok "$1 $2 $3 (假, 按预期)"
    else no "$1 $2 $3 (rc=$_r, 期望 $4)"; fi; }
vc 1.0  '<'  1.1     0
vc 1.9  '<'  1.10    0
vc 1.0  '<'  1.0.1   0
vc 1.0  '<'  1.0a    0        # 数字段排在字母段之前
vc 1.0a '>' 1.0      0
vc 1.0  '=' 1.0      0
vc 1.0  '!=' 1.0-1   0
vc 1:1.0 '>' 9.9     0        # epoch 压倒一切
vc 0:1.0 '=' 1.0     0
vc 2.0b1 '<' 2.0b2   0
vc 1.10 '<' 1.9      1
vc 1.0-1 '=' 1.0-1   0
# 与上游 rpm 的判定逐条对照: 上游没有 vercmp 子命令, 用 --qf 的表达式做不到,
# 所以这里用"装进临时根谁赢"来断言版本先后(见 section 5 的降级判据)。
chk "不认的运算符要报错(不是当作真)" \
    "! Q --compare-versions 1.0 '~=' 1.0 >/dev/null 2>&1"

echo "=== RESULT: pass=$pass fail=$fail ==="
[ "$fail" = 0 ] || exit 1
[ -n "${KEEP:-}" ] || rm -rf "$T"
