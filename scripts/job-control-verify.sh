#!/bin/sh
# job-control-verify.sh - 验 parlz-sh 把终端前台权交给子 shell(GNU bash)。
#
# 现象(修前): body 里起的 shell 再跑 `bash` 会打
#   bash: cannot set terminal process group (-2): Inappropriate ioctl for device
#   bash: no job control in this shell
# 根因: parlz-sh 让子进程留在**自己的进程组**里,bash 想 setpgid+tcsetpgrp
# 抢前台就失败 —— 它不是会话主,没人把终端让它接管。现在 sh.c 每条前台命令
# (含整条管道)都放进独立进程组,并把 tcsetpgrp 指过去,命令结束后再收回。
#
# 判据(都是终态,不看提示语):
#   1) 日志里**不得**出现上面那两条告警(出现即 FAIL);
#   2) bash 真跑起来了(打印出 $BASH_VERSION);
#   3) bash 里 Ctrl+C 真能打断 sleep($? = 130)—— 证明前台权确实交给了它。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/job-control-verify.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
W=/home/jgzyes/job-control-verify
rm -rf "$W"; mkdir -p "$W"
LOG=$W/serial.log FIFO=$W/in.fifo
for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs"; do
    [ -f "$f" ] || { echo "缺 $f(先 build-kernel.sh)"; exit 1; }
done
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"

qemu-system-x86_64 -m 1024M -nographic -no-reboot \
  -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 install.skip=1 login.skip=1" \
  <"$FIFO" >"$LOG" 2>&1 &
QPID=$!

i=0
while [ $i -lt 120 ]; do
    grep -qa "type commands directly" "$LOG" && break
    grep -qa "kernel panic" "$LOG" && break
    i=$((i+1)); sleep 1
done
echo ">>> parlz-sh 就绪(第 ${i}s)"

printf 'bash\n' >&3
sleep 4
printf 'echo INSIDE_BASH=$BASH_VERSION\n' >&3
sleep 3
printf 'sleep 60\n' >&3
sleep 3
printf '\003' >&3          # Ctrl+C:此刻前台组是 bash 的
sleep 3
printf 'echo JC_RC=$?\n' >&3
sleep 4
exec 3>&-
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true

echo "--- 关键行 ---"
grep -a "bash\|INSIDE_BASH\|JC_RC=\|job control\|process group" "$LOG" | tail -10

FAIL=""
grep -aq "cannot set terminal process group" "$LOG" \
    && FAIL="$FAIL:仍有 cannot set terminal process group"
grep -aq "no job control in this shell" "$LOG" \
    && FAIL="$FAIL:仍有 no job control in this shell"
grep -aq "INSIDE_BASH=5" "$LOG" \
    || FAIL="$FAIL:没进 bash(没打印 BASH_VERSION)"
grep -aq "JC_RC=13[07]" "$LOG" \
    || FAIL="$FAIL:bash 里 Ctrl+C 没拿到 130/137"

if [ -n "$FAIL" ]; then
    echo "job-control-verify: FAIL ->$FAIL"
    tail -25 "$LOG"
    exit 1
fi
echo "job-control-verify: PASS(bash 接管终端无告警, 里面 Ctrl+C 仍有效)"
