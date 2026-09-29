#!/bin/sh
# ctrl-c-verify.sh - 验 Ctrl+C 能强制退出前台命令。
# 根因(已修): busybox-init 的 ::sysinit 子进程不是 session leader, /dev/console
# 成不了控制终端, ISIG 产生的 SIGINT 发不到前台进程组。sh.c 现在交互启动时
# setsid + TIOCSCTTY 接管 /dev/console。
# 判据: 跑 sleep 60 -> 发 0x03 -> 提示符回来, echo $? 打印 130/137(被信号打断)。
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/ctrl-c-verify.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
W=/home/jgzyes/ctrl-c-verify
rm -rf "$W"; mkdir -p "$W"
LOG=$W/serial.log; FIFO=$W/in.fifo
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"          # 先占住写端, 避免 qemu <FIFO 与"等日志"互等

qemu-system-x86_64 -m 1024M -nographic -no-reboot \
  -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 install.skip=1 login.skip=1" \
  <"$FIFO" >"$LOG" 2>&1 &
QPID=$!

i=0
while [ $i -lt 300 ]; do
    grep -qa "type commands directly" "$LOG" && break
    grep -qa "kernel panic" "$LOG" && break
    i=$((i+1)); sleep 1
done
echo ">>> shell 就绪(第 ${i}s)"

printf 'sleep 60\n' >&3
sleep 3
grep -aq "^sleep$\|sleep 60" "$LOG" && echo ">>> sleep 已在前台跑"
printf '\003' >&3        # Ctrl+C
sleep 3
printf 'echo RC=$?\n' >&3
sleep 4
exec 3>&-
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true

echo "--- 关键行 ---"
grep -a "sleep 60\|RC=\|type commands directly\|Parlz shell" "$LOG" | tail -8

if grep -aq "RC=13[07]" "$LOG"; then
    echo "ctrl-c-verify: PASS(前台命令被 Ctrl+C 打断, 退出码 $((128+0)) 系)"
else
    echo "ctrl-c-verify: FAIL(没有看到 RC=130/137 —— Ctrl+C 仍杀不掉前台命令)"
    tail -20 "$LOG"
    exit 1
fi
