#!/bin/sh
# pkg-guest-e2e.sh - 四个包管理器在**真 guest 里**跑通端到端(带网络)。
#
# 宿主侧 *-verify.sh 已经拿上游工具逐条核过格式与求解; 这里只回答另一件事:
# 在 Parlz 内核 + 我们的 initramfs 里, apt/yum 能不能真联网拉索引、下载、
# 校验, 再把文件落到盘上、状态库登记对、卸载干净。仓库由宿主 python3 起在
# QEMU user-NAT 的宿主地址 10.0.2.2 上。
#
# 喂进 guest 的每条命令都是**单条简单命令**: 交互 shell 是 /bin/parlz-sh,
# 它没有 && / if / for(AGENTS.md 里记着), 所以断言靠"串口日志里出现某行"。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c \
#         "sh /mnt/f/Linux/Parlz/scripts/pkg-guest-e2e.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
W=/home/jgzyes/parlz-pkg-e2e
LOG=$W/serial.log
DEB_PORT=${DEB_PORT:-18801}
RPM_PORT=${RPM_PORT:-18802}
FAIL=""
pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'PASS  %s\n' "$1"; }
no()  { fail=$((fail+1)); printf 'FAIL  %s\n' "$1"; }

for t in dpkg-deb apt-ftparchive rpmbuild createrepo_c python3 mkfs.vfat mcopy; do
    command -v $t >/dev/null 2>&1 || { echo "!! 宿主缺 $t"; exit 1; }
done
for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs"; do
    [ -f "$f" ] || { echo "!! 缺 $f(先跑 build-kernel.sh)"; exit 1; }
done

rm -rf "$W"; mkdir -p "$W"
KVM=""; [ -w /dev/kvm ] && KVM="-enable-kvm"

# ---------- 1) 造两个真仓库(一个 deb, 一个 rpm) ----------
S=$W/stage
mkdir -p "$S/usr/local/bin" "$S/etc/e2edemo"
printf '#!/bin/sh\necho GUEST-DEMO-RAN\n' > "$S/usr/local/bin/e2edemo"
chmod 755 "$S/usr/local/bin/e2edemo"
printf 'cfg=1\n' > "$S/etc/e2edemo/e2e.conf"
printf '#!/bin/sh\necho GUEST-RPM-RAN\n' > "$S/usr/local/bin/e2erpm"
chmod 755 "$S/usr/local/bin/e2erpm"

DEB=$W/deb; RPM=$W/rpm
mkdir -p "$DEB/pool" "$DEB/dists/stable/main/binary-all" \
         "$DEB/dists/stable/main/binary-amd64" "$RPM"
DEBI=$W/debsrc; mkdir -p "$DEBI/bin" "$DEBI/DEBIAN"
printf '#!/bin/sh\necho GUEST-DEMO-RAN\n' > "$DEBI/bin/e2edemo"
chmod 755 "$DEBI/bin/e2edemo"
printf 'cfg=1\n' > "$DEBI/e2e.conf"
mkdir -p "$DEBI/etc/e2edemo"; cp "$DEBI/e2e.conf" "$DEBI/etc/e2edemo/"
cat > "$DEBI/DEBIAN/control" <<'EOF'
Package: e2edemo
Version: 1.0
Architecture: all
Maintainer: Parlz <pkg@parlz.com>
Installed-Size: 2
Priority: optional
Section: misc
Depends: e2elib
Description: guest end-to-end demo
 pulls e2elib
EOF
mkdir -p "$W/libsrc/bin" "$W/libsrc/DEBIAN"
printf '#!/bin/sh\necho E2ELIB\n' > "$W/libsrc/bin/e2elib"
chmod 755 "$W/libsrc/bin/e2elib"
cat > "$W/libsrc/DEBIAN/control" <<'EOF'
Package: e2elib
Version: 2.5
Architecture: all
Maintainer: Parlz <pkg@parlz.com>
Installed-Size: 1
Description: dependency of e2edemo
 leaf
EOF
dpkg-deb -Zgzip --build "$W/libsrc" "$DEB/pool/e2elib_2.5_all.deb" >/dev/null
dpkg-deb -Zgzip --build "$DEBI"   "$DEB/pool/e2edemo_1.0_all.deb" >/dev/null
( cd "$DEB" && apt-ftparchive packages pool > dists/stable/main/binary-all/Packages )
cp "$DEB/dists/stable/main/binary-all/Packages" \
   "$DEB/dists/stable/main/binary-amd64/Packages"
gzip -kf "$DEB/dists/stable/main/binary-amd64/Packages"
( cd "$DEB" && apt-ftparchive release dists/stable > dists/stable/Release )

RB=$W/rpmbuild
# 注意: dash 没有 {a,b} 花括号展开(这脚本跑在 sh 上), 必须逐个列出来
mkdir -p "$RB/SPECS" "$RB/RPMS" "$RB/BUILD" "$RB/BUILDROOT" "$RB/SOURCES" "$RB/SRPMS"
cat > "$RB/SPECS/e2erpm.spec" <<'EOS'
Name: e2erpm
Version: 3.1
Release: 2
Summary: guest rpm end-to-end
License: GPL-2.0
BuildArch: x86_64
Requires: e2elib
%description
guest end-to-end rpm
%pre
echo "RPM-PRE $1" > /tmp/rpm-pre-ran
%post
echo "RPM-POST $1" > /tmp/rpm-post-ran
%install
mkdir -p %{buildroot}/usr/local/bin
printf '#!/bin/sh\necho GUEST-RPM-RAN\n' > %{buildroot}/usr/local/bin/e2erpm
chmod 755 %{buildroot}/usr/local/bin/e2erpm
%files
%defattr(-,root,root,-)
/usr/local/bin/e2erpm
EOS
cat > "$RB/SPECS/e2elib.spec" <<'EOS'
Name: e2elib
Version: 1.4
Release: 1
Summary: leaf for the rpm test
License: GPL-2.0
BuildArch: x86_64
%description
leaf
%install
mkdir -p %{buildroot}/usr/local/bin
printf '#!/bin/sh\necho E2ELIB-RPM\n' > %{buildroot}/usr/local/bin/e2elibrpm
chmod 755 %{buildroot}/usr/local/bin/e2elibrpm
%files
/usr/local/bin/e2elibrpm
EOS
for s in e2elib e2erpm; do
    rpmbuild --define "_topdir $RB" --define "debug_package %{nil}" \
             --define "_rpmdir $RB/RPMS" -bb "$RB/SPECS/$s.spec" \
        >"$W/rpmbuild-$s.log" 2>&1 \
    || { echo "!! rpmbuild $s 失败"; tail -5 "$W/rpmbuild-$s.log"; exit 1; }
done
find "$RB/RPMS" -name '*.rpm' -exec cp {} "$RPM/" \;
createrepo_c "$RPM" >"$W/createrepo.log" 2>&1 || {
    echo "!! createrepo_c 失败"; tail -5 "$W/createrepo.log"; exit 1; }

python3 -m http.server $DEB_PORT --directory "$DEB" --bind 0.0.0.0 \
    >"$W/httpd-deb.log" 2>&1 &
H1=$!
python3 -m http.server $RPM_PORT --directory "$RPM" --bind 0.0.0.0 \
    >"$W/httpd-rpm.log" 2>&1 &
H2=$!
trap 'kill $H1 $H2 2>/dev/null' EXIT INT TERM

echo "=== 2) 起 guest(-nographic 串口序), 联网后喂命令 ==="
FIFO=$W/in.fifo; rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"
timeout -k 10 480 qemu-system-x86_64 $KVM -m 1024M -nographic -no-reboot \
  -serial mon:stdio \
  -kernel "$IMG/parlz-bzImage" \
  -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 login.skip=1" \
  <"$FIFO" >"$LOG" 2>&1 &
Q=$!

wait_for() {
    i=0
    while [ $i -lt "$2" ]; do
        grep -qa "$1" "$LOG" 2>/dev/null && return 0
        grep -qa "Kernel panic" "$LOG" 2>/dev/null && { echo "  (kernel panic)"; return 2; }
        kill -0 $Q 2>/dev/null || return 3
        i=$((i+1)); sleep 1
    done
    return 1
}
feed() { printf '%s\n' "$1" >&3; }

if ! wait_for "type commands directly" 300; then
    echo "FAIL: 没等到 guest shell 就绪"; tail -30 "$LOG"; FAIL=1
fi

if [ -z "$FAIL" ]; then
    # body 已经 ifc dhcp 过; 等一下再确认
    feed "busybox sleep 4"
    feed "dpkg --version"
    feed "rpm --version"
    feed "apt --version"
    feed "yum --version"
    feed "echo E2E-VERSIONS-DONE"
    if ! wait_for "^E2E-VERSIONS-DONE" 60; then
        echo "FAIL: 四个管理器没有一个打出版本"; tail -25 "$LOG"; FAIL=1
    fi
fi

if [ -z "$FAIL" ]; then
    # ---- apt: 写源 → update → 装(带依赖) → 真跑装出来的程序 ----
    feed "echo deb [trusted=yes] http://10.0.2.2:$DEB_PORT stable main >> /etc/apt/sources.list"
    feed "apt update"
    feed "echo E2E-UPDATED"
    if ! wait_for "^E2E-UPDATED" 90; then
        echo "FAIL: apt update 没跑完"; tail -30 "$LOG"; FAIL=1
    fi
    feed "apt -y install e2edemo"
    feed "echo E2E-APT-DONE"
    if ! wait_for "^E2E-APT-DONE" 180; then
        echo "FAIL: apt install 没跑完"; tail -30 "$LOG"; FAIL=1
    fi
    feed "e2edemo"
    feed "e2elib"
    feed "dpkg -l"
    feed "echo E2E-APT-VERIFIED"
    if ! wait_for "^E2E-APT-VERIFIED" 60; then
        echo "FAIL: 装完的验证段没跑完"; tail -25 "$LOG"; FAIL=1
    fi
    for m in GUEST-DEMO-RAN E2ELIB "ii e2edemo" "ii e2elib"; do
        if grep -qa "$m" "$LOG"; then ok "apt 装的包真能跑/登记: $m"
        else no "缺: $m"; fi
    done
    feed "apt -y remove e2edemo"
    feed "echo E2E-APT-REMOVED"
    wait_for "^E2E-APT-REMOVED" 90 >/dev/null || no "apt remove 没跑完"
    feed "e2edemo"
    feed "echo E2E-AFTER-REMOVE"
    wait_for "^E2E-AFTER-REMOVE" 40 >/dev/null
    # 卸掉之后 e2edemo 不该还能跑(可能打 not found 或类似)
    if sed -n "/^E2E-APT-REMOVED\$/,/^E2E-AFTER-REMOVE\$/p" "$LOG" | grep -qa GUEST-DEMO-RAN; then
        no "apt remove 之后 e2edemo 仍可执行(没删干净)"
    else
        ok "apt remove 之后 e2edemo 已不可执行"
    fi
fi

if [ -z "$FAIL" ]; then
    # ---- yum: 写仓库 → makecache → 装(带依赖) → 真跑 → 卸 ----
    feed "echo [e2e] >> /etc/yum.repos.d/e2e.repo"
    feed "echo name=e2e >> /etc/yum.repos.d/e2e.repo"
    feed "echo baseurl=http://10.0.2.2:$RPM_PORT >> /etc/yum.repos.d/e2e.repo"
    feed "echo enabled=1 >> /etc/yum.repos.d/e2e.repo"
    feed "echo gpgcheck=0 >> /etc/yum.repos.d/e2e.repo"
    feed "yum makecache"
    feed "yum -y install e2erpm"
    feed "echo E2E-YUM-DONE"
    if ! wait_for "^E2E-YUM-DONE" 240; then
        echo "FAIL: yum 段没跑完"; tail -40 "$LOG"; FAIL=1
    fi
    feed "e2erpm"
    feed "e2elibrpm"
    feed "rpm -qa"
    feed "echo E2E-YUM-VERIFIED"
    if ! wait_for "^E2E-YUM-VERIFIED" 60; then
        echo "FAIL: yum 验证段没跑完"; tail -25 "$LOG"; FAIL=1
    fi
    for m in GUEST-RPM-RAN E2ELIB-RPM "e2erpm-3.1-2.x86_64" "e2elib-1.4-1.x86_64"; do
        if grep -qa "$m" "$LOG"; then ok "yum 装的包真能跑/登记: $m"
        else no "缺: $m"; fi
    done
    feed "rpm -e e2erpm-3.1-2.x86_64"
    feed "echo E2E-RPM-REMOVED"
    wait_for "^E2E-RPM-REMOVED" 90 >/dev/null || no "rpm -e 没跑完"
    feed "e2erpm"
    feed "echo E2E-RPM-AFTER"
    wait_for "^E2E-RPM-AFTER" 40 >/dev/null
    if sed -n "/^E2E-RPM-REMOVED\$/,/^E2E-RPM-AFTER\$/p" "$LOG" | grep -qa GUEST-RPM-RAN; then
        no "rpm -e 之后 e2erpm 仍可执行(没删干净)"
    else
        ok "rpm -e 之后 e2erpm 已不可执行"
    fi
    # 卸载脚本段应该留下过痕迹(%pre/%post 写的是 /tmp 里两个标记文件)
    feed "cat /tmp/rpm-post-ran"
    feed "echo E2E-SCRIPT-DONE"
    wait_for "^E2E-SCRIPT-DONE" 40 >/dev/null
    if grep -qa "RPM-POST 1" "$LOG"; then ok "包内 %post 在 guest 里真跑过"
    else no "包内 %post 没跑(状态: 见日志)"; fi
fi

# ---------- 收尾 ----------
feed "/bin/busybox sync"
feed "echo E2E-SYNCED"
wait_for "^E2E-SYNCED" 30 >/dev/null
kill $Q 2>/dev/null
sleep 1
kill -9 $Q 2>/dev/null
exec 3>&-

echo "=== RESULT: pass=$pass fail=$fail ==="
[ "$fail" = 0 ] || { echo "--- 串口尾 40 行:"; tail -40 "$LOG"; exit 1; }
[ -n "${KEEP:-}" ] || rm -rf "$W"
