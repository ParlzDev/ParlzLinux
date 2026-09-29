#!/bin/sh
# verify-user-install.sh - 验收"**在运行中的系统里**把系统装到硬盘"(README 方式一)。
#
# 阶段 1(在系统内装盘): 与 scripts/boot-install.sh **同一套 QEMU 参数**起系统
#   -kernel/-initrd + 目标盘(可写 /dev/vda) + 引导镜像盘(只读 /dev/vdb)
#   cmdline: parlz.bootimg=/dev/vdb install.skip=1 login.skip=1
#   起来后是普通 shell, 通过 FIFO 往里喂命令(等价人手敲 `install`),
#   装完直接 kill QEMU(人手上则敲 reboot, 再由 boot-disk.sh 从盘启动)。
#   断言: "rootfs copied"(cpfs 真成功, 不是走 initramfs 兜底) + "install complete"
#
# 阶段 2(从装好的盘自启): 与 scripts/boot-disk.sh 同参数, 裸引导。
#   断言必须落到"**已安装根里的 shell 真能跑命令**" —— 只看 pivot_root OK
#   会被骗: 换根换成一个空目录照样打印 OK, 然后立刻 (shell exited)。
#   首启要求设置用户名/密码(写进已安装根的 /etc/parlz-auth), 顺带证明
#   换过去的是可写的真盘根。
#
# 阶段 3(宿主复核): 把盘的分区 2 挂回来核对文件/软链/权限
#   (scripts/check-installed-root.sh)。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/verify-user-install.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
DISK=${PARLZ_DISK:-/home/jgzyes/parlz-disk.img}
LOG=/home/jgzyes/parlz-user-install.log
W=/home/jgzyes/parlz-verify
FAIL=""

# 安装源必须是**串口序 + 本轮内核**的引导镜像: 本脚本一路 -nographic, 装到盘上
# 的 syslinux.cfg 与 vmlinuz 都取自这份镜像 —— 用交付序(vga)装出来的盘, 首启
# 登录提示只上屏幕、串口什么都没有; 用**旧内核**装出来的盘, 自启跑的是旧
# initramfs(判据看着在验新代码, 其实跑的是上一轮的 body/login)。
# 这两条判据都在 serial-media.sh 里(内容 + `::/vmlinuz` 与本轮产物比对),
# 所以这里**无条件**调它, 让守卫去决定要不要重建。
. /mnt/f/Linux/Parlz/scripts/serial-media.sh
parlz_ensure_serial_media || exit 1
BOOTIMG=$PARLZ_SERIAL_BF

for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs" "$BOOTIMG"; do
    [ -f "$f" ] || { echo "缺 $f(先跑 build-2404.sh 或 build-kernel.sh)"; exit 1; }
done
rm -rf "$W" && mkdir -p "$W"
KVM=""; [ -w /dev/kvm ] && KVM="-enable-kvm"

# wait_for <串口里要出现的关键字> <超时秒> ; 0=等到, 1=超时
wait_for() {
    i=0
    while [ $i -lt "$2" ]; do
        grep -qa "$1" "$LOG" 2>/dev/null && return 0
        grep -qa "kernel panic" "$LOG" 2>/dev/null && { echo "  (kernel panic)"; return 2; }
        i=$((i+1)); sleep 1
    done
    return 1
}

echo "=== 阶段 1: 在系统内 install(boot-install.sh 同参数) ==="
rm -f "$DISK" "$LOG"
truncate -s 512M "$DISK"           # 从零装: 目标盘必须是空盘
FIFO=$W/in.fifo; rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"        # 同阶段 2: 先占住写端, 见下面的注释
qemu-system-x86_64 $KVM -m 1024M -nographic -no-reboot \
  -serial mon:stdio \
  -device ac97 \
  -kernel "$IMG/parlz-bzImage" \
  -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 parlz.bootimg=/dev/vdb install.skip=1 login.skip=1" \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -drive file="$BOOTIMG",if=virtio,format=raw,readonly=on \
  <"$FIFO" >"$LOG" 2>&1 &
Q=$!
if ! wait_for "type commands directly" 240; then
    echo "FAIL: 没等到 guest shell 就绪"; tail -25 "$LOG"; FAIL=1
fi
if [ -z "$FAIL" ]; then
    printf 'install\n' >&3
    if ! wait_for "install complete" 900; then
        echo "FAIL: install 没跑完"; tail -40 "$LOG"; FAIL=1
    fi
fi
exec 3>&-
kill $Q 2>/dev/null; wait $Q 2>/dev/null
[ -z "$FAIL" ] || { echo "阶段 1 FAIL, 串口日志尾:"; tail -30 "$LOG"; exit 1; }
echo "阶段 1 PASS: 在系统内 install 完成"
grep -aE "引导镜像源|MBR written|VBR @LBA|mkfs ext2|rootfs copied|install complete" "$LOG" | head -8
grep -qa "rootfs copied" "$LOG" \
    || { echo "FAIL: cpfs 没报 rootfs copied(装出来的根会是空的)"; exit 1; }
sleep 2

echo ""
echo "=== 阶段 2: 从装好的盘自启(boot-disk.sh 同参数) ==="
# 首启会在**已安装的根**上要求设置用户名/密码(写 /etc/parlz-auth)——
# 顺带证明换根后的根是可写的真盘, 不是 initramfs。喂完凭证再让 shell
# 跑一条命令, 输出回到串口才算"装好的系统能用"。
rm -f "$LOG" "$FIFO"; mkfifo "$FIFO"
# ★ 先以**读写**方式占住 FIFO: `qemu < $FIFO` 的 open 会阻塞到出现写端,
# 而写端在"等日志出现"的循环之后 —— 两边互等就死锁(日志永远是空的)。
exec 3<>"$FIFO"
qemu-system-x86_64 $KVM -m 1024M -nographic -no-reboot \
  -serial mon:stdio \
  -device ac97 \
  -drive file="$DISK",if=virtio,format=raw,cache=none -boot c \
  <"$FIFO" >"$LOG" 2>&1 &
Q=$!
sh /mnt/f/Linux/Parlz/scripts/guest-first-login.sh "$LOG" "$FIFO" tester parlz123 300 \
    || echo "阶段 2 FAIL: 首启登录没喂进去"
if ! wait_for "Parlz shell" 180; then
    echo "阶段 2 FAIL: 认证后没进 shell"
fi
sleep 3
printf 'echo installed-root-ok $USER\n' >&3
wait_for "installed-root-ok" 60 || echo "阶段 2 FAIL: shell 里跑命令没回显"
# ★ 落盘再断电: 这个 QEMU 是直接 kill 的(等价拔电源), 页缓存里的东西不会自己
#   写下去 —— 阶段 3 在宿主挂盘复核时会看不到刚写的 /etc/parlz-auth(踩过)。
#   parlz-sh 没有 &&, 两条分开敲。
printf '/bin/busybox sync\n' >&3
sleep 3
printf 'echo SYNCED\n' >&3
wait_for "SYNCED" 30 || echo "  (警告: guest 没确认 sync, 阶段 3 可能看到旧盘面)"
sleep 2
exec 3>&-
grep -aE "Booting from Hard|SYSLINUX|Parlz 0\.1\.0|vda: |root device|mounted|pivot_root|installed:|已创建用户|Parlz shell|installed-root-ok|No such file" \
    "$LOG" | head -16
kill $Q 2>/dev/null; wait $Q 2>/dev/null
if grep -qa "pivot_root OK, now running on installed root" "$LOG" \
   && grep -qa "已创建用户 tester" "$LOG" \
   && grep -qa "installed-root-ok tester" "$LOG" \
   && ! grep -qa "kernel panic" "$LOG"; then
    echo "阶段 2 PASS: 磁盘自启 → 换根 → 首启建用户 → 已安装根 shell 可执行命令"
else
    echo "阶段 2 FAIL"; tail -30 "$LOG"; exit 1
fi

echo ""
echo "=== 阶段 3: 宿主复核已安装的根(文件/软链/权限) ==="
sh /mnt/f/Linux/Parlz/scripts/check-installed-root.sh "$DISK" || exit 1

echo ""
echo "=== 阶段 4: 装好的盘 + -kernel 起(boot-install.sh 场景的 reboot) ==="
# 不带 root=(syslinux.cfg 才有): body 得自己从 /dev 里挑出**能挂成 ext 的
# 分区**当根。早先取 head -1 会挑中 FAT16 引导分区 /dev/vda1, 挂上后既
# pivot 不了、又让 install 误判"目标盘已挂载"而拒绝重装。
rm -f "$LOG"
qemu-system-x86_64 $KVM -m 1024M -nographic -no-reboot \
  -serial mon:stdio \
  -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 login.skip=1" \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  </dev/null >"$LOG" 2>&1 &
Q=$!
wait_for "pivot_root OK, now running on installed root" 240 \
    || echo "阶段 4 FAIL: 没换根"
kill $Q 2>/dev/null; wait $Q 2>/dev/null
grep -aE "root device|mounted|pivot_root|installed:" "$LOG" | head -6
if grep -qa "pivot_root OK, now running on installed root /dev/vda2" "$LOG"; then
    echo "阶段 4 PASS: 无 root= 也认出了已安装的根(装完 reboot 直接进系统)"
else
    echo "阶段 4 FAIL"; tail -25 "$LOG"; exit 1
fi

echo ""
echo "verify-user-install: PASS(在系统内装盘 → 磁盘自启 → 装好的根可用)"
