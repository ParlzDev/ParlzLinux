#!/bin/bash
# 内部脚本: 跑 pm-verify.sh(避免外层 pkill 匹配到自身命令行被杀)
pkill -f "python3 -m http.server 8765" 2>/dev/null
pkill -f "qemu-system-x86_64" 2>/dev/null
sleep 1
rm -f /home/jgzyes/pm-verify.log
sh /mnt/f/Linux/Parlz/scripts/pm-verify.sh > /home/jgzyes/pmv.out 2>&1
echo "pvrc=$?"
grep -E "PASS:|FAIL:|PM_ALL|PM_FAIL|pm install|pm-server" /home/jgzyes/pmv.out | head -20
tail -1 /home/jgzyes/pmv.out
