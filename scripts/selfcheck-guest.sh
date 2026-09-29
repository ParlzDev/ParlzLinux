#!/bin/sh
# selfcheck-guest.sh - "命令地毯": 在真 guest 里把 shell 与核心命令过一遍。
#
# 不是断言脚本(那些在 *_verify*.sh 里), 而是**找 bug 的探测**: 每条命令前打
# `===== CMD: <名字>` 标记, 命令后打 `----- rc=$?`, 输出整段落盘。人工/后续
# 审查时一眼能看出哪条命令报了错(not found / cannot / error / 空输出)。
#
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/selfcheck-guest.sh [串口日志路径]"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
OUT=${1:-/home/jgzyes/selfcheck-guest.log}
W=/home/jgzyes/selfcheck-guest
rm -rf "$W"; mkdir -p "$W"
[ -f "$IMG/parlz-bzImage" ] || { echo "缺 $IMG/parlz-bzImage"; exit 1; }

FIFO=$W/in.fifo
rm -f "$FIFO" "$OUT"; mkfifo "$FIFO"
exec 3<>"$FIFO"
qemu-system-x86_64 -m 1024M -nographic -no-reboot \
  -kernel "$IMG/parlz-bzImage" -initrd "$IMG/parlz-initramfs" \
  -append "console=ttyS0,115200 install.skip=1 login.skip=1 net.skip=1" \
  <"$FIFO" >"$OUT" 2>&1 &
QP=$!

wait_for() {
    i=0
    while [ "$i" -lt $(( ${2:-60} * 5 )) ]; do
        grep -qa "$1" "$OUT" 2>/dev/null && return 0
        grep -qa "Kernel panic" "$OUT" 2>/dev/null && return 2
        i=$((i + 1)); sleep 0.2
    done
    return 1
}

wait_for "type commands directly" 120 || { echo "没进 shell"; tail -20 "$OUT"; kill $QP; exec 3>&-; exit 1; }
sleep 1

# 每条命令: 打标记 → 喂命令 → 等一会儿。命令之间用 `echo` 分隔符隔开,
# 免得前一条的尾巴粘到后一条上(parlz-sh 的提示符也是一行)。
run() {   # run <名字> <命令行>
    printf 'echo ===== CMD: %s\n' "$1" >&3
    sleep 0.4
    printf '%s\n' "$2" >&3
    sleep "${3:-0.8}"
}

run pwd          'pwd'
run cd-tmp       'cd /tmp'
run pwd2         'pwd'
run cd-back      'cd ..'
run pwd3         'pwd'
run echo-basic   'echo hello world'
run redir-out    'echo abc > f1.txt'
run cat-f1       'cat f1.txt'
run redir-append 'echo def >> f1.txt'
run cat-f1b      'cat f1.txt'
run redir-in     'cat < f1.txt'
run pipe-grep    'cat f1.txt | /bin/busybox grep abc'
run grep-file    'grep abc f1.txt'
run grep-n       'grep -n abc f1.txt'
run rc-ok        'echo RC1=$?'
run notfound     'nosuchcmd123'
run rc-127       'echo RC2=$?'
run var-brace    'X=hello; echo ${X}world'
run var-prefix   'X=1 /bin/busybox env'
run quote-double 'echo "a  b"'
run quote-single "echo 'c  d'"
run ls-l         'ls -l f1.txt'
run cp           'cp f1.txt f2.txt'
run cat-f2       'cat f2.txt'
run ln-s         'ln -s f1.txt link1'
run readlink     'ls -l link1'
run chmod        'chmod 700 f2.txt'
run mv           'mv f2.txt f3.txt'
run cat-f3       'cat f3.txt'
run rm           'rm f3.txt'
run rm-missing   'rm nosuchfile-xyz'
run rc-rm        'echo RC3=$?'
run mkdir        'mkdir d1'
run mkdir-p      'mkdir d1'          # 重复创建: 期望报错而不是崩
run mkdir-nested 'mkdir d1/a/b'
run rmdir?       'rm d1/a/b'
run tree         'tree /tmp'
run file         'file f1.txt'
run df           'df'
run free         'free'
run ps           'ps'
run dmesg1       'dmesg | /bin/busybox head -3'
run which        'which ls'
run busybox-ls   '/bin/busybox ls /tmp'
run busybox-uname '/bin/busybox uname -a'
run sh-c         'sh -c "echo from-c"'
run script-file  'echo "echo script-ran" > s.sh'
run script-run   'sh s.sh'
run user-cmd     'user'
run user-rm-none 'user rm nosuch'
run rc-user      'echo RC4=$?'
run pm-help      'pm'
run pm-version   'pm --version'
run login-help   '/bin/login --help'
run rc-login     'echo RC5=$?'
run empty-line   ''
run hash-cmd     'echo done-all'

wait_for "done-all" 30
sleep 2
exec 3>&-
kill $QP 2>/dev/null; wait $QP 2>/dev/null
echo "自检输出: $OUT ($(wc -l < "$OUT") 行)"
