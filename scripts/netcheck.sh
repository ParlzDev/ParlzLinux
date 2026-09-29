#!/bin/sh
# netcheck.sh - QEMU 起 guest,把测试脚本喂给 sh 执行,收集全部输出
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
IMG=/mnt/f/Linux/Parlz/images
D=/tmp/parlz-net-disk.img
LOG=/tmp/parlz-net.log
rm -f "$D" "$LOG"
dd if=/dev/zero of="$D" bs=1M count=256 2>/dev/null

# 测试命令(通过 sh 脚本模式跑,喂给 guest)
cat > /tmp/net-test.sh << 'EOF'
echo "=== TEST: 网络命令实测 ==="
ifconfig
ifc auto 10.0.2.15 255.255.255.0 10.0.2.2
ifconfig
curl -s http://10.0.2.2/ 2>&1 | head -3
echo "=== curl https 测试 ==="
curl -v https://example.com/ 2>&1 | head -15
echo "=== shell 管道+重定向测试 ==="
echo pipe_test > /tmp/ptest.txt
cat /tmp/ptest.txt | cat
echo "=== 全部测试完成 ==="
EOF

# 把测试脚本塞进 initramfs 不好,改用:QEMU 启动后,shell 自动 cat 不出,
# 所以直接把测试命令写进 cmdline 不可行(长度)。改为:挂一个额外 initramfs
# 最简单:改 boot 脚本,启动后 shell 里手动跑。这里直接交互,用 telnet 输入。
