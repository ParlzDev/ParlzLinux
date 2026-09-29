#!/bin/sh
# guest-first-login.sh - 非交互地替 guest 完成"首次进入设置用户名/密码"。
#
# 磁盘自启的 syslinux.cfg 不带 login.skip(真机首启本来就要设密码),
# 所以自动化脚本必须把凭证喂进串口, 否则永远停在 "Username:" 提示上。
# 配合 `qemu -nographic -serial mon:stdio < stdin 命名管道` 使用。
#
# 用法: sh guest-first-login.sh <串口日志> <stdin FIFO> <用户> <密码> [超时秒]
#   0 = 已喂入; 1 = 没等到提示; 2 = kernel panic
# 调用方必须在起 QEMU **之前** `exec 3<>"$FIFO"` 占住写端:
# `qemu < $FIFO` 的 open 会阻塞到出现写端, 而本脚本要等的日志正是
# QEMU 起来后才写的 —— 不先占住就互等死锁。
set -u
LOG=$1
FIFO=$2
USER=$3
PASS=$4
TMO=${5:-300}

send() {
    # tee 打开 FIFO 写端会阻塞到 QEMU 这个读端在收; timeout 兜底,
    # 免得 QEMU 已经退出还把整个验收挂死。
    printf '%s\n' "$1" | timeout 30 tee "$FIFO" >/dev/null
}

i=0
while [ "$i" -lt "$TMO" ]; do
    grep -qa "Username:" "$LOG" 2>/dev/null && break
    grep -qa "kernel panic" "$LOG" 2>/dev/null && { echo "guest-first-login: kernel panic"; exit 2; }
    i=$((i + 1)); sleep 1
done
grep -qa "Username:" "$LOG" 2>/dev/null \
    || { echo "guest-first-login: $TMO 秒内没等到 Username: 提示"; exit 1; }

# 首启是三问(用户名/密码/确认); 已设置过的盘只问两问, 多敲的一行
# 落到 shell 里只是 "未知命令", 不影响后续判定。
sleep 2
send "$USER"
sleep 3
send "$PASS"
sleep 3
send "$PASS"
echo "guest-first-login: 凭证已喂入(用户 $USER)"
