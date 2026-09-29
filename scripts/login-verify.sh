#!/bin/sh
# login-verify.sh - guest 侧 login 认证验收:
# 注入 /etc/parlz-auth 后, init 应在进交互 shell 前校验用户名/密码。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/login-verify.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/mnt/f/Linux/Parlz/images
LOG="$IMG/parlz-login.log"
rm -f "$LOG"

KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"

# 场景:无 /etc/parlz-auth(首次进入)。login 子进程 stdin 指 /dev/console:
# - 交互场景(-serial stdio):isatty(0)==1,走 Username/Password 认证。
# - 自动化(file: 捕获):QEMU 串口对 guest 仍是 tty,login 会停在
#   Username: 提示 —— 本脚本在 -append 加 login.skip=1 让 login 直接放行,
#   验证 "init: login check" 标记 + shell 正常启动的完整链路。
#   交互认证本身由宿主 pty 测试 scripts/login-pty-test.py 覆盖。
timeout -k 10 120 qemu-system-x86_64 \
  $KVM -m 2048M -nographic -no-reboot \
  -serial file:"$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200 earlycon login.skip=1" \
  -initrd "$IMG/parlz-initramfs" >/dev/null 2>&1 || true

echo "=== verify login markers in $LOG ==="
pass=0
fail=0
# 判据: 走到登录环节的标记是 body 打的那行 "type commands directly"
# (旧版 grep 的 "init: login check" 早已不存在 —— 判据过期会把好链路报成 FAIL)
grep -aq "type commands directly" "$LOG" && { echo "PASS: 走到登录环节"; pass=$((pass+1)); } || { echo "FAIL: 没走到登录环节"; fail=$((fail+1)); }
grep -aq "login: cmdline 带 login.skip,跳过认证" "$LOG" && { echo "PASS: login.skip 放行"; pass=$((pass+1)); } || { echo "FAIL: 无 login.skip 放行标记"; fail=$((fail+1)); }
grep -aq "Parlz shell" "$LOG" && { echo "PASS: shell 正常启动(认证放行后)"; pass=$((pass+1)); } || { echo "FAIL: 无 shell 标记(被登录阻塞?)"; fail=$((fail+1)); }
echo "=== PASS=$pass FAIL=$fail ==="
[ $fail -eq 0 ] && echo "LOGIN_VERIFY_OK" || echo "LOGIN_VERIFY_FAIL"
