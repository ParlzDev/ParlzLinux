#!/bin/bash
# PWeb PHP-CGI test runner (run inside WSL).
set -u
for p in $(pgrep -f "pweb pweb.conf"); do kill -9 "$p" 2>/dev/null; done
sleep 1
ss -ltn 2>/dev/null | grep -qE "8080|8099" && { echo "ports busy, aborting"; exit 1; }
cd /mnt/f/Linux/PWeb
rm -f /tmp/pweb_stdout.log /tmp/pweb_stderr.log
nohup ./pweb pweb.conf >/tmp/pweb_stdout.log 2>/tmp/pweb_stderr.log &
PWEB_PID=$!
echo "pweb pid=$PWEB_PID"
sleep 1
echo "=== startup ==="
cat /tmp/pweb_stdout.log
echo ""
echo "===== PHP TEST BATTERY ====="
echo "--- 1. GET /cgi-bin/test.php?name=ZCode&lang=C ---"
curl -s --max-time 20 "http://127.0.0.1:8080/cgi-bin/test.php?name=ZCode&lang=C"
echo ""
echo "--- 2. POST /cgi-bin/test.php (form data) ---"
curl -s --max-time 20 -X POST -H "Content-Type: application/x-www-form-urlencoded" \
  --data "user=admin&action=save&msg=hi pweb" "http://127.0.0.1:8080/cgi-bin/test.php"
echo ""
echo "--- 3. static regression: index + about ---"
curl -s -o /dev/null -w "index=%{http_code} " --max-time 5 "http://127.0.0.1:8080/"
curl -s -o /dev/null -w "about=%{http_code}\n" --max-time 5 "http://127.0.0.1:8080/about.html"
echo "--- 4. 404 for missing .php ---"
curl -s -i --max-time 5 "http://127.0.0.1:8080/cgi-bin/does-not-exist.php" | head -3
echo "--- 5. pweb stderr (CGI diagnostics) ---"
cat /tmp/pweb_stderr.log
echo "=== end ==="
