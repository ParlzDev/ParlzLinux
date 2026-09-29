#!/bin/sh
# dbg-vga.sh - 诊断磁盘自启: 用 QEMU monitor 导出 VGA 文本缓冲, 看 syslinux
# 在屏幕上写了什么(-nographic 只转发串口, syslinux 走 INT10/VGA 看不到)。
# 用法: wsl -d Ubuntu-24.04 -u root -e sh /mnt/f/Linux/Parlz/scripts/dbg-vga.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
DISK=${1:-/home/jgzyes/parlz-e2e-disk.img}
MON=/tmp/parlz-mon.sock
SERIAL=/home/jgzyes/parlz-dbg-serial.log
rm -f "$MON" "$SERIAL"
qemu-system-x86_64 -machine pc -m 512 -smp 2 \
  -drive file="$DISK",if=virtio,format=raw,cache=none \
  -boot c -nographic \
  -monitor unix:"$MON",server,nowait \
  </dev/null >"$SERIAL" 2>&1 &
QPID=$!
sleep 15
printf 'xp /4000xb 0xb8000\n' | nc -U -q 3 "$MON" > /tmp/parlz-vga.raw 2>/dev/null || true
printf 'info registers\n' | nc -U -q 3 "$MON" > /tmp/parlz-regs.txt 2>/dev/null || true
kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
echo "=== 串口日志(全部) ==="
cat "$SERIAL"
echo ""
echo "=== VGA 文本(解析 0xb8000) ==="
python3 - <<'PY'
import re
txt = open("/tmp/parlz-vga.raw", errors="ignore").read()
vals = []
for line in txt.splitlines():
    m = re.match(r"^([0-9a-f]{8}):\s+(.*)$", line.strip())
    if not m:
        continue
    for tok in m.group(2).split():
        if re.fullmatch(r"[0-9a-f]{2}", tok):
            vals.append(int(tok, 16))
# VGA 文本: 偶数下标是字符, 奇数是属性
out = []
for i in range(0, len(vals) - 1, 2):
    c = vals[i]
    out.append(chr(c) if 32 <= c < 127 else ("\n" if c in (0x0a, 0x0d) else " "))
s = "".join(out)
# 每行 80 字符, 去掉整行空白
for i in range(0, len(s), 80):
    line = s[i:i+80].rstrip()
    if line.strip():
        print("  |" + line)
PY
