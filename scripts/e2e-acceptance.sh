#!/bin/sh
# e2e-acceptance.sh - 一轮式 QEMU 验收:本地起 HTTP/HTTPS 服务器喂 guest,
# 跑 nettest.sh 的验收项,然后逐项 grep 串口日志。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/e2e-acceptance.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
LOG="$IMG/parlz-e2e.log"
DOCROOT=/tmp/e2e-docroot

# 1) 文档根 + 自签证书
rm -rf "$DOCROOT" /tmp/e2e-cert.pem /tmp/e2e-key.pem
mkdir -p "$DOCROOT"
echo "PARLZ_HTTP_BODY" > "$DOCROOT/hello.txt"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj "/CN=10.0.2.2" -keyout /tmp/e2e-key.pem -out /tmp/e2e-cert.pem \
  >/dev/null 2>&1

# 2) 本地 HTTP(8080) + HTTPS(8443) 服务器(后台,脚本结束统一杀)
python3 /mnt/f/Linux/Parlz/scripts/e2e-https-server.py 8443 "$DOCROOT" &
HS_PID=$!
(cd "$DOCROOT" && python3 -m http.server 8080 >/dev/null 2>&1) &
HTTP_PID=$!
trap 'kill $HS_PID $HTTP_PID 2>/dev/null' EXIT
sleep 1

# 宿主侧先自检两个端口(失败早退,不给 guest 背锅)
curl -s -m 5 http://127.0.0.1:8080/hello.txt | grep -q PARLZ_HTTP_BODY \
  || { echo "HOST_HTTP_FAIL"; exit 1; }
curl -sk -m 5 https://127.0.0.1:8443/hello.txt | grep -q PARLZ_HTTP_BODY \
  || { echo "HOST_HTTPS_FAIL"; exit 1; }

# 3) QEMU 启动 guest(user 网络,10.0.2.2 = host)
rm -f "$LOG"
KVM=""
[ -w /dev/kvm ] && KVM="-enable-kvm"
timeout -k 10 150 qemu-system-x86_64 \
  $KVM -m 512M -nographic -no-reboot \
  -serial file:"$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200 earlycon" \
  -netdev user,id=n0 -device e1000,netdev=n0 \
  -initrd "$IMG/parlz-initramfs" >/dev/null 2>&1 || true

# 4) 逐项判定
pass=0; fail=0
check() {
  if grep -aq "$1" "$LOG"; then
    echo "PASS $2"; pass=$((pass+1))
  else
    echo "FAIL $2 ($1)"; fail=$((fail+1))
  fi
}
check "Parlz user-space"       内核启动进入用户空间
check "ARR=b c"                bash 数组
check "BRACE=1 BRACE=2"        bash 大括号扩展
check "E1"                     bash echo -e 行1
check "GLOBQ=0"                bash [[ ]] 通配匹配
check "PROC_OK"                bash 进程替换
check "mode of '/tmp/perm.txt': 0644 -> 0000" "chmod 000 生效（root 仍可读）"
check "CHMOD_RESTORE_OK"       chmod 644 恢复读取
check "opkg version 0.8.0"     opkg 真实后端
check "opkg version 0.8.0"     ppm→opkg 委托
check "HTTP_OK"                curl HTTP 明文
check "HTTPS_OK"               curl HTTPS/PazeSSL
check "WGET_OK"                wget HTTP
echo "=== RESULT: pass=$pass fail=$fail ==="
[ "$fail" = 0 ]
