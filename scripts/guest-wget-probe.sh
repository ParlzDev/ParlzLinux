#!/bin/sh
# guest-wget-probe.sh - QEMU guest 内验证 wget/curl 下载进度条(宿主慢速 HTTP server)。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/guest-wget-probe.sh
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
LOG=/home/jgzyes/guest-wget.log
PORT=8899
rm -f "$LOG"

# host 慢速 server(监听 0.0.0.0:$PORT, 分块慢发逼出多次进度回调)。
# 监听 0.0.0.0 而非 127.0.0.1:QEMU user-mode 下 guest 经 NAT 出去到达的是
# WSL 侧 IP,hostfwd 无法绑到 QEMU 内部 10.0.2.2(guest 访问不到 127.0.0.1)。
python3 -c "
import http.server, time
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'D' * 400000
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        for i in range(0, len(body), 8192):
            self.wfile.write(body[i:i+8192]); self.wfile.flush(); time.sleep(0.02)
    def log_message(self, *a): pass
http.server.HTTPServer(('0.0.0.0', $PORT), H).serve_forever()
" &
SRV=$!
sleep 1

# guest 经 ifc 配网(10.0.2.15/24, gw 10.0.2.2)后, 对 WSL host $PORT 跑 wget。
# guest 默认出向 NAT 经 WSL 宿主 IP($WSLIP,由 hostname -I 取),直达 host 上
# 监听 0.0.0.0:$PORT 的服务;hostfwd 仅做 guest->host 端口映射(非必须)。
WSLIP=$(hostname -I | awk '{print $1}')
echo "=== guest wget 目标: http://$WSLIP:$PORT (host 0.0.0.0:$PORT) ==="
# 把 wget 探针写进 /init.d 让 init 跑完网配后自动执行(或手工在串口输入)
# 这里用 -nographic + 串口日志抓 wget 输出;init 已有 ifc auto。
# 在 cmdline 带 parlz.install 会触发安装,不要带;直接靠 /nettest.sh 或手工。
# 简化:guest 起来后在 shell 里手工 wget $WSLIP:$PORT。此处只验证二进制含进度。
timeout 240 qemu-system-x86_64 -m 512 -nographic -no-reboot \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200 earlycon" \
  -initrd "$IMG/parlz-initramfs" \
  -netdev user,id=n0 \
  -device e1000,netdev=n0 \
  -serial file:"$LOG" \
  -monitor none </dev/null 2>&1 | head -3
kill $SRV 2>/dev/null

echo "=== guest 串口日志(节选) ==="
if [ -f "$LOG" ]; then
  tr '\r' '\n' < "$LOG" | sed 's/\x1b\[[0-9;?]*[a-zA-Z]//g' \
    | grep -av '^$' | grep -aE 'Parlz|parlz>|ifc|network|wget|curl|%|K/s|Run /init' | head -40
else
  echo "日志未生成(QEMU 启动参数问题)"
fi
