#!/bin/sh
# toolchain-verify.sh — guest 内工具链端到端验收: 临时把 /tooltest.sh 注入
# 现成 initramfs, 起 QEMU(串口), 等 guest 自测 gcc/clang 出 TC_ALL_OK 判定。
# 用法: wsl -d Ubuntu-26.04 -e sh /mnt/f/Linux/Parlz/scripts/toolchain-verify.sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
[ -f "$IMG/parlz-initramfs" ] || { echo "缺 $IMG/parlz-initramfs, 先跑 build-userland.sh"; exit 1; }
[ -f "$IMG/parlz-bzImage" ] || { echo "缺 $IMG/parlz-bzImage, 先跑 build-kernel.sh"; exit 1; }

# 现成 initramfs 是 cpio(gzip), 把 tooltest.sh 追加进去(不重打 CMake)
TMP=/home/jgzyes/parlz-init-tc-work
rm -rf "$TMP"; mkdir -p "$TMP"
OUT=/home/jgzyes/parlz-init-tc.cpio.gz
cd "$TMP"
gunzip -c "$IMG/parlz-initramfs" | cpio -idm 2>/dev/null
cp /mnt/f/Linux/Parlz/userland/tooltest.sh.in tooltest.sh
# WSL /tmp 与 cwd 的 9P/路径异常可能让 cp 写出 0 字节, 校验并就地补写
sz=$(wc -c < tooltest.sh)
[ "$sz" -gt 100 ] || { echo ">>> 错误: tooltest.sh 注入为 0 字节($sz), 中止" >&2; exit 1; }
chmod +x tooltest.sh
echo "    tooltest.sh 注入: $sz bytes"
find . | LC_ALL=C sort | cpio -o -H newc 2>/dev/null | gzip -9 > "$OUT"
cd /; rm -rf "$TMP"
echo ">>> 注入 /tooltest.sh 的 initramfs: $(ls -lh "$OUT" | awk '{print $5}')"

KVM=""; [ -w /dev/kvm ] && KVM="-enable-kvm"
LOG=/home/jgzyes/tc-verify.log
rm -f "$LOG"
qemu-system-x86_64 $KVM -m 4096M -nographic -no-reboot \
  -serial "file:$LOG" \
  -kernel "$IMG/parlz-bzImage" \
  -append "console=ttyS0,115200" \
  -initrd "$OUT" &
QPID=$!

# 最多等 240s 出判定
OK=""
for i in $(seq 1 240); do
  if grep -q "TC_ALL_OK\|TC_FAIL\|init: tooltest.sh finished" "$LOG" 2>/dev/null; then
    sleep 3   # 等串口落完
    OK=1; break
  fi
  kill -0 "$QPID" 2>/dev/null || { sleep 2; grep -q "TC_ALL_OK\|TC_FAIL" "$LOG" 2>/dev/null && { OK=1; break; }; }
  sleep 1
done
[ -n "$OK" ] && kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== guest 串口判定 ==="
grep -E "TC_ALL_OK|TC_FAIL|PASS|FAIL|tooltest|gcc|g\+\+|clang" "$LOG" 2>/dev/null | grep -v "^$" | head -40
if grep -q "TC_ALL_OK" "$LOG" && ! grep -q "TC_FAIL" "$LOG"; then
  echo ">>> PASS: guest 内 gcc/clang 动态+静态全通"
  exit 0
fi
echo ">>> FAIL: 见上方串口输出"
exit 1
