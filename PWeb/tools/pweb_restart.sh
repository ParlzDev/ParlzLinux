#!/bin/bash
set -u
# Kill all pweb
for p in $(pgrep -f "pweb pweb.conf" 2>/dev/null); do
  kill -9 "$p" 2>/dev/null
done
sleep 1
# Verify ports free
ss -ltn | grep -qE "8080|8099" && { echo "ports still busy"; exit 1; }
cd /mnt/f/Linux/PWeb
nohup ./pweb pweb.conf > /tmp/pweb_stdout.log 2> /tmp/pweb_stderr.log &
echo "pweb pid=$!"
sleep 1
echo "=== stdout ==="
cat /tmp/pweb_stdout.log
