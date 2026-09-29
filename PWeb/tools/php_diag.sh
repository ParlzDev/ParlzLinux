#!/bin/bash
# PWeb PHP-CGI test runner (run inside WSL).
set -u
# kill any running pweb
for p in $(ps -C pweb -o pid= 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
fuser -k 8080/tcp 8099/tcp 2>/dev/null
sleep 1
cd /mnt/f/Linux/PWeb
# clean rebuild
rm -f main.o conn.o config.o http_parser.o handlers.o util.o mime.o log.o pweb
make >/dev/null 2>&1
[ -x ./pweb ] || { echo "BUILD FAILED"; make 2>&1 | grep -E "error|Error" | head; exit 1; }
rm -f /tmp/pweb.out
nohup ./pweb pweb.conf >/tmp/pweb.out 2>&1 &
sleep 1
echo "===== PHP TEST BATTERY ====="
echo "--- 1. GET /cgi-bin/test.php?name=ZCode&lang=C ---"
curl -s --max-time 20 "http://127.0.0.1:8080/cgi-bin/test.php?name=ZCode&lang=C"
echo ""
echo "--- 2. POST /cgi-bin/test.php (form data) ---"
curl -s --max-time 20 -X POST -H "Content-Type: application/x-www-form-urlencoded" \
  --data "user=admin&action=save&msg=hi pweb" \
  "http://127.0.0.1:8080/cgi-bin/test.php"
echo ""
echo "--- 3. static regression: index + about ---"
curl -s -o /dev/null -w "index=%{http_code} " --max-time 5 "http://127.0.0.1:8080/"
curl -s -o /dev/null -w "about=%{http_code}\n" --max-time 5 "http://127.0.0.1:8080/about.html"
echo "--- 4. 404 for missing .php ---"
curl -s -i --max-time 5 "http://127.0.0.1:8080/cgi-bin/does-not-exist.php" | head -3
echo "--- 5. pweb.out diagnostics ---"
cat /tmp/pweb.out
echo "===== END ====="
