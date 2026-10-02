#!/bin/bash
# parlz-boot-body.sh - busybox-init ::sysinit 的启动主体。
# 由 parlz-boot.sh(shim)用 /bin/bash 直跑, 全 POSIX。
# 交互性: stdin 是 /dev/console(哪块屏由 cmdline 的 console= 顺序决定);
#         自动化脚本(stdin=pipe/file, 非 tty) → login 放行不阻塞。
#
# 关键: busybox-init 的 sysinit 子进程 fd 0/1/2 不在控制台(内核
# populate initramfs 阶段 "unable to open an initial console" 后
# console fd 失效, 子进程 echo 全丢)。必须把 0/1/2 重指 /dev/console,
# 之后所有输出(banner/探测/install/login/shell 提示符)与交互认证都
# 落到串口上。
#
# ★ 顺序: **先挂 devtmpfs 再重接**(踩过 —— 反了的话 /dev/console 还
# 不存在, 重接失败, 整个 body 连同 install 的输出全部丢失, 串口上只
# 剩 login.c 自己的输出, 看起来像"install 没跑")。
# /dev/console 具体是哪块屏由 cmdline 里**最后**一个 console= 决定(见
# scripts/console-cfg.sh): 交付介质是 tty0(VMware/真机键盘能输), QEMU
# -nographic 的验收介质是 ttyS0。重指逻辑对两者一样, 宿主 chroot 测时可能
# 打不开 /dev/console, 重接失败就保持原 fd, 不致命。
# 用 open+dup2 语义(bash: 3<> 读写打开)而非 >> 追加 —— 对 tty 的 O_APPEND
# 在某些内核下异常。
/bin/busybox mount -t proc proc /proc 2>/dev/null
/bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
/bin/busybox mount -t devtmpfs devtmpfs /dev -o mode=0755 2>/dev/null
/bin/busybox mount -t tmpfs tmpfs /tmp -o mode=1777 2>/dev/null
# devtmpfs 挂上后 /dev/console 才存在; 少数情况下节点稍后才注册, 重试几次
for _try in 1 2 3 4 5; do
    [ -c /dev/console ] && break
    /bin/busybox sleep 1
done
if exec 3<>/dev/console 2>/dev/null; then
    exec 0<&3 1>&3 2>&3
    exec 3<&- 3>&-
    echo "body: console reattached"
fi

# --- 环境(busybox-init sysinit 子进程不带完整环境) ---
export PATH=/usr/bin:/usr/sbin:/usr/local/bin:/bin:/sbin
export HOME=/root
export TERM=vt220
export TERMINFO_DIRS=/etc/terminfo:/usr/share/terminfo
export SHELL=/bin/bash
export LS_COLORS=1
export CFLAGS=-no-pie
export CXXFLAGS=-no-pie
export LDFLAGS=-no-pie
# 工具链是 pm 装的**可选包**, 目录名带版本(装的是 gcc-13/llvm-18 还是
# gcc-15/llvm-21 取决于宿主产包时的版本) —— 这里按 /opt/toolchain 下实际存在
# 的目录探测, 不把版本号焊进启动脚本(焊死过一次: 换宿主后装了包也找不到头)。
TCG=""; TCL=""; CXXI=""
for d in /opt/toolchain/gcc-*;  do [ -d "$d" ] && TCG="$d"; done
for d in /opt/toolchain/llvm-*; do [ -d "$d" ] && TCL="$d"; done
for d in /usr/include/c++/*;    do [ -d "$d" ] && CXXI="$d"; done
LDP="/lib/toolchain"
[ -n "$TCG" ] && LDP="$LDP:$TCG/lib"
[ -n "$TCL" ] && LDP="$LDP:$TCL/lib"
# ★ 这三行是**兜底**, 不是动态编译成立的条件。包内(build-toolchain.sh 的
#   dev 名字层 + build-pm-packages.sh 的三棵多架构软链树)已经把
#   libc.so/libm.so/libstdc++.so/libgcc_s.so 与 /usr/lib/x86_64-linux-gnu、
#   /lib/x86_64-linux-gnu、/lib 都铺好了, 所以不设这些变量也应该能
#   编译并跑起动态产物 —— 判据见 scripts/toolchain-dyn-verify.sh
#   (chroot + env -i, 显式断这三个变量为空)。
#   留着是为了喂给老镜像里装上去的旧包, 别拿它当"已经修好了"的证据。
export LD_LIBRARY_PATH="$LDP"
export LIBRARY_PATH=/lib/toolchain:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu
CP=""
[ -n "$TCG" ] && CP="$TCG/include"
[ -n "$CXXI" ] && CP="${CP:+$CP:}$CXXI"
export CPATH="$CP"
export C_INCLUDE_PATH="$TCG"
if [ -n "$TCG" ] || [ -n "$TCL" ]; then
    echo "init: 工具链就位 ${TCG:-无gcc} ${TCL:-无llvm} C++头=${CXXI:-无}"
fi
# pm 源: 这里原本无条件 `export PM_FEED=http://10.0.2.2:8765`, 把盘上
# /etc/pm/feeds.conf 直接盖掉 —— 交付介质里 pm 永远去敲一台不存在的开发机。
# 默认不导出, 让 pm 自己读 /etc/pm/feeds.conf; 要临时换源用 cmdline 的
# parlz.feed=<url>(自动化), 或 shell 里自己 export PM_FEED。
FI=`/bin/busybox grep -o 'parlz.feed=[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
if [ -n "$FI" ]; then
    export PM_FEED="${FI#parlz.feed=}"
    echo "init: pm feed = $PM_FEED (cmdline)"
fi
export USER=root
export HOSTNAME=parlz
# 内核 nodename 一直是默认值 "(none)"(没人调 sethostname), 于是 GNU bash 的
# 提示符 \h 显示 root@(none) —— 它取 gethostname(), 不认 $HOSTNAME。设一次,
# 让自研 sh 与 bash 的提示符一致。
/bin/busybox hostname parlz 2>/dev/null

# --- 引导镜像源(cmdline parlz.bootimg=) ---
# 装到磁盘时 install 要往目标盘写引导分区, 镜像本体在外部(ISO / 附加
# 只读盘 / 文件), 见 install.c open_boot_image。把 cmdline 参数导出成
# PARLZ_BOOT_IMG 让 install 直接用; 不给也能自动探测(非目标盘的
# FAT16 引导盘)。busybox grep 有 -o, userland 自研 grep 没有。
BI=`/bin/busybox grep -o 'parlz.bootimg=[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
if [ -n "$BI" ]; then
    export PARLZ_BOOT_IMG="${BI#parlz.bootimg=}"
    echo "init: boot image source = $PARLZ_BOOT_IMG (cmdline)"
fi

# --- 挂载链已在文件开头完成(必须在 console 重接之前), 这里只做提示 ---
echo "=== Parlz user-space (busybox-init) ==="

# --- root 设备探测: cmdline root= 优先, 否则扫 /dev 上的分区。
# userland 极简 grep 不支持 -o/正则 —— 全部走 /bin/busybox。
#
# ★ 只有 ext2/ext4 才算"已安装的真根": 磁盘上分区 1 是 FAT16 引导分区,
# 早先 `head -1` 取第一个分区正好取到它 —— 装完 reboot 时 cmdline 不带
# root=(boot-install.sh 那条路径), 于是把 /dev/vda1 挂到 /mnt 当根候选,
# 既 pivot 不了(留在 initramfs), 又让 install 以为目标盘正被挂载而拒绝
# 重装。逐个试, 谁能在 ext 下挂起来谁才是根。
#
# ★ 候选**不能用 `ls /dev` 去 parse**: body 的 stdout 是 /dev/console(一个
# tty), busybox ls 于是按**多列**排版输出("vda vda1 vda2" 挤在一行),
# `grep -E '^(vd|sd)[a-z]+[0-9]+$'` 一行都匹配不上 —— 候选恒为空, 已装好的
# 盘被当成空盘, `/install.d` 又重跑一遍安装(实测踩过两次)。改成 shell 全局
# 展开, 不依赖任何命令的输出格式。
# ★ 另外 devtmpfs 是**逐个、异步**注册分区节点的: 内核日志已打 `vda: vda1
# vda2`, /dev 里当时可能只有 vda1。所以"取候选 + 逐个试挂"整轮重试,
# 连整盘节点都没有(真没插盘)时立刻退出, 不白等。 ---
ROOTARG=`/bin/busybox grep -o 'root=[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
ROOTARG=${ROOTARG#root=}
ROOTDEV=""
_r=0
while [ $_r -lt 5 ]; do
    _r=$((_r + 1))
    CAND="$ROOTARG"
    if [ -z "$CAND" ]; then
        for _n in /dev/vd[a-z]*[0-9] /dev/sd[a-z]*[0-9]; do
            [ -b "$_n" ] && CAND="$CAND $_n"
        done
    fi
    for _dev in $CAND; do
        [ -b "$_dev" ] || continue
        if /bin/mount -t ext4 "$_dev" /mnt 2>/tmp/.mnt.err; then
            ROOTDEV="$_dev"; echo "init: mounted $_dev at /mnt (ext4)"
            break
        fi
        if /bin/mount -t ext2 "$_dev" /mnt 2>/tmp/.mnt.err; then
            ROOTDEV="$_dev"; echo "init: mounted $_dev at /mnt (ext2)"
            break
        fi
    done
    [ -n "$ROOTDEV" ] && break
    _wholereg=""
    for _n in /dev/vd[a-z] /dev/sd[a-z]; do
        [ -b "$_n" ] && _wholereg="$_n"
    done
    [ -n "$_wholereg" ] || break      # 根本没插盘, 不用等
    /bin/busybox sleep 2
done
MERR=`/bin/cat /tmp/.mnt.err 2>/dev/null`
if [ -z "$ROOTDEV" ]; then
    echo "init: 没有可挂载的 ext 根(盘没装过 / 分区未格式化?), 留在 initramfs"
    [ -n "$MERR" ] && echo "init: 最后一次挂载尝试: $MERR"
fi
echo "init: root device probe -> ${ROOTDEV:-<none>}"

# pivot_root: 仅 ext 根已挂 /mnt 才 pivot(换到安装好的真根, initramfs
# 留在 /mnt/oldroot)。挂的是 vfat(引导分区)时不 pivot。
if [ -n "$ROOTDEV" ]; then
    if /bin/busybox grep -q '/mnt ext' /proc/mounts 2>/dev/null; then
        echo "init: pivot_root -> /mnt (ext root)"
        # PUT_OLD(/mnt/oldroot)必须已存在, 否则 pivot_root 直接 ENOENT
        # ("pivot_root: No such file or directory"), 一直停在 initramfs。
        # cpfs 拷 rootfs 时跳过 /mnt /proc /sys /dev /tmp, 所以这里要建。
        /bin/busybox mkdir -p /mnt/oldroot /mnt/proc /mnt/sys /mnt/dev /mnt/tmp
        if /bin/busybox pivot_root /mnt /mnt/oldroot; then
            # 换根后 initramfs 的 proc/sys/dev/tmp 挂载点留在 oldroot,
            # 新根里是空目录 —— 必须重新挂, 否则 /proc/cmdline、/dev/* 全丢。
            /bin/busybox mount -t proc proc /proc 2>/dev/null
            /bin/busybox mount -t sysfs sysfs /sys 2>/dev/null
            /bin/busybox mount -t devtmpfs devtmpfs /dev -o mode=0755 2>/dev/null
            /bin/busybox mount -t tmpfs tmpfs /tmp -o mode=1777 2>/dev/null
            echo "init: pivot_root OK, now running on installed root $ROOTDEV"
        else
            echo "init: pivot_root failed, continuing on initramfs"
        fi
    fi
fi

# --- 安装介质(CD/ISO): 挂到 /cdrom, 让 install 能取引导镜像 ---
# install 往目标盘写引导分区, 镜像本体在安装介质上(ISO 内 /boot/fat16.img)。
# 内核已编 iso9660/sr(CD-ROM), 直接 mount 即可。无 CD 时静默跳过。
#
# ★ 必须 -o ro: CD-ROM 是只读设备, 不带 MS_RDONLY 时内核按可写打开,
# sr 驱动拒绝 → 内核打 "/dev/sr0: Can't open blockdev", mount 失败。
# 实测踩过(util-linux 会对 iso9660 自动加只读, 我们的极简 mount 不会)。
for _cd in /dev/sr0 /dev/sr1 /dev/sr2; do
    [ -b "$_cd" ] || continue
    /bin/busybox mkdir -p /cdrom
    if /bin/mount -o ro -t iso9660 "$_cd" /cdrom 2>/tmp/.cd.err; then
        echo "init: ISO 介质 $_cd 已挂到 /cdrom (ro)"
        if [ -f /cdrom/boot/fat16.img ]; then
            echo "init: 引导镜像 /cdrom/boot/fat16.img 就位"
            [ -n "$PARLZ_BOOT_IMG" ] || export PARLZ_BOOT_IMG=/cdrom/boot/fat16.img
        else
            echo "init: 警告: /cdrom 无 boot/fat16.img(装盘会缺引导镜像)"
        fi
        break
    else
        echo "init: mount $_cd -> /cdrom 失败: $(/bin/cat /tmp/.cd.err 2>/dev/null)"
    fi
done

# --- ISO 自举: /install.d 且无 install-done 标记(整盘 LBA 1)→ 跑安装 ---
INSTALLED=0
DISKWHOLE=$ROOTDEV
N=0
while [ -n "$DISKWHOLE" ] && [ $N -lt 4 ]; do
    LEN=${#DISKWHOLE}
    CH=${DISKWHOLE:LEN-1:1}
    N=$((N+1))
    case $CH in
        [0-9]) DISKWHOLE=${DISKWHOLE:0:LEN-1} ;;
        *) break ;;
    esac
done
if [ -n "$DISKWHOLE" ] && [ -b "$DISKWHOLE" ]; then
# ★ dd/tr 都在 pm-trim 名单里(默认系统里命令名不存在), 必须走绝对 busybox
MARK=`/bin/busybox dd if="$DISKWHOLE" bs=512 skip=1 count=1 2>/dev/null | /bin/busybox tr -d '\0'`
    case "$MARK" in
        *"parlz install done"*) INSTALLED=1 ;;
    esac
fi
# cmdline install.skip: 跳过自动安装, 留给用户在 shell 里手动跑 `install`
# (在系统内装盘的场景: 想把系统装到哪块盘、什么时候装由用户决定)。
SKIP_AUTO=0
CI=`/bin/busybox grep -o 'install.skip[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
[ -n "$CI" ] && SKIP_AUTO=1
if [ "$SKIP_AUTO" = "1" ]; then
    echo "init: cmdline 带 install.skip, 跳过自动安装"
    echo "init: 手动安装: install [目标盘]  (不带给参数则自动探测第一块可写盘)"
    echo "init: 装完 reboot 从磁盘启动"
elif [ -x /install.d ] && [ "$INSTALLED" = "0" ]; then
    echo "init: running /install.d (auto-install, 300s 看门狗)"
    # fork + 看门狗: install 正常 <300s(写 64MiB FAT + ext2 + cpfs
    # rootfs)。若探测死等(无盘/异常)超时 kill, 不阻塞进 shell。
    ( /bin/sh /install.d & IWD=$!
      # 计数器循环而不是 seq: seq 也在 pm-trim 名单里(默认系统没这个命令名)
      _wd=0
      while [ $_wd -lt 300 ]; do
          _wd=$((_wd + 1))
          kill -0 $IWD 2>/dev/null || break
          /bin/busybox sleep 1
      done
      kill -0 $IWD 2>/dev/null && { kill -TERM $IWD 2>/dev/null; echo "init: install.d 超时(300s), 已中断"; } )
    wait
    echo "init: install.d finished"
elif [ -x /install.d ]; then
    echo "init: install already done (marker present), skipping"
fi

# --- 网络: 交付介质上默认走 DHCP(失败不致命, shell 里可手动配) ---
# 这里原本是焊死的静态地址 `ifc auto 10.0.2.15 255.255.255.0 10.0.2.2`。
# 那三件套全是 QEMU user-NAT 的约定: 在 VMware/真机上 ifc 照样回成功、
# body 照样打 "init: network up", 但那块网络谁也连不通(/etc/resolv.conf
# 烘的也是 10.0.2.3)。用户侧的表现是"有网卡的假象 + pm 拉不到包"。
# 现在: cmdline 给 parlz.ip= 才用静态(给没有 DHCP 的自动化环境留路),
# 否则问 DHCP —— QEMU user-NAT 内置 DHCP, 拿回来的还是 10.0.2.15/24,
# 验收脚本的判据不变, 真机也终于能真的连上。
NI=`/bin/busybox grep -o 'parlz.ip=[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
NM=`/bin/busybox grep -o 'parlz.netmask=[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
NG=`/bin/busybox grep -o 'parlz.gw=[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
NASKIP=`/bin/busybox grep -o 'net.skip[^ ]*' /proc/cmdline 2>/dev/null | /bin/busybox head -1`
NET_OK=0
if [ -n "$NASKIP" ]; then
    echo "init: cmdline 带 net.skip, 不配网络"
elif [ -n "$NI" ]; then
    IPV="${NI#parlz.ip=}"
    MASKV=${NM#parlz.netmask=}
    [ -n "$NM" ] || MASKV=255.255.255.0
    GWV=${NG#parlz.gw=}
    echo "init: configuring network via /bin/ifc static $IPV (cmdline parlz.ip=)..."
    if [ -n "$GWV" ]; then
        /bin/ifc auto "$IPV" "$MASKV" "$GWV" && NET_OK=1
    else
        /bin/ifc auto "$IPV" "$MASKV" && NET_OK=1
    fi
else
    echo "init: configuring network via /bin/ifc dhcp..."
    /bin/ifc dhcp && NET_OK=1
fi
if [ "$NET_OK" = 1 ]; then
    echo "init: network up"
else
    echo "init: network config failed, use ifc/ifconfig in shell"
fi

# --- 自测钩子(验证脚本注入, 缺失静默跳过) ---
# ★ tooltest/pmtest 必须**自动跑**: 这两条的验收脚本(scripts/toolchain-verify.sh
#   / scripts/pm-verify.sh)只 grep "TC_ALL_OK"/"PM_ALL_OK" 与 "finished"。
#   以前这里只打 "present (run manually)" —— 那是 init.c 时代的残留:
#   正常启动早已是 busybox-init + body(内核默认走 /sbin/init, 见
#   AGENTS/CONFIG), init.c 里那两个钩子根本不会被执行, 于是验收脚本
#   白等到超时, 报出来的 FAIL 与产品无关(实测: pm-verify 等 27 分钟零输出)。
#   看门狗用 `kill -0` 轮询 + busybox sleep(不依赖 timeout applet, 它可能被裁)。
run_hook(){ # $1=脚本 $2=秒数上限
    [ -x "$1" ] || return 0
    echo "init: running $1 ($2s 看门狗)"
    /bin/bash "$1" &
    _hp=$!
    _n=0
    while kill -0 "$_hp" 2>/dev/null; do
        _n=$((_n + 1))
        if [ "$_n" -gt "$2" ]; then
            echo "init: $1 超时(${2}s), kill"
            kill -9 "$_hp" 2>/dev/null
            break
        fi
        /bin/busybox sleep 1
    done
    wait "$_hp" 2>/dev/null
    echo "init: ${1##*/} finished (status $?)"
}
run_hook /tooltest.sh 900        # 工具链在 rootfs 里(自带工具链的构建)
run_hook /pmtest.sh 1500         # 要先 pm install 工具链: GB 级下载 + 12 条编译
[ -x /nettest.sh ]    && echo "init: /nettest.sh present (run manually)"
[ -x /audiotest.sh ]  && echo "init: /audiotest.sh present (run manually)"

cd /root 2>/dev/null

INST_STATE="no"
[ "$INSTALLED" = "1" ] && INST_STATE="yes"
echo "=== Parlz boot ready ==="
echo "  root device: ${ROOTDEV:-<none>}"
echo "  installed:   $INST_STATE"
echo "  type commands directly, or 'login' to authenticate."
echo ""

# --- 登录钩子: 直接 /bin/login(stdin 继承 busybox-init 给 sysinit
# 子进程的 tty; 自动化非 tty)。
#
# ★ 认证失败**不许**放行(踩过): 这里原来不看 login 的退出码, 于是 3 次
# 输错之后照样往下走 into shell, 而且 USER 是拿 `head -1 /etc/parlz-auth`
# 取的 —— 多账户下还会张冠李戴(登进来的是谁都没问过)。现在的规矩:
#   - login 退出码非 0 = 拒绝 → 隔 2 秒重新弹认证, **永远不进 shell**;
#   - 认证通过的用户名由 login 写进 /tmp/.parlz-login-user(子进程 setenv
#     传不回父进程), 这里读出来导出 USER 后立刻删掉。
# 自动化(non-tty / login.skip)路径下 login 直接返回 0, 不会卡在重试上。 ---
if [ -x /bin/login ]; then
    while :; do
        /bin/login && break
        echo "login: 认证失败,2 秒后重试(Ctrl-Alt-Del 可重启)"
        /bin/busybox sleep 2
    done
    AUTH_USER=`/bin/busybox cat /tmp/.parlz-login-user 2>/dev/null`
    /bin/busybox rm -f /tmp/.parlz-login-user 2>/dev/null
    if [ -n "$AUTH_USER" ]; then
        export USER="$AUTH_USER"
        echo "login: 用户 $AUTH_USER 认证通过"
    fi
fi
if [ -z "$USER" ]; then export USER=root; fi
if [ -z "$HOSTNAME" ]; then export HOSTNAME=parlz; fi

# 进交互 shell: shell 自身会按 $TERM raw 模式读 0/1(已由 body 开头
# 重指到 /dev/console), 直接 exec 即可。退出后 sysinit 结束, 不自动重拉。
/bin/parlz-sh
echo ""
echo "(shell exited)"
