#!/bin/bash
# PWeb PHP-CGI test suite (run inside WSL).
set -u
cd /mnt/f/Linux/PWeb

# kill all pweb instances
for p in $(ps -C pweb -o pid= 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
sleep 1

# clean rebuild
rm -f main.o conn.o config.o http_parser.o handlers.o util.o mime.o log.o pweb
make >/tmp/pweb_make.log 2>&1 || { echo "BUILD FAILED"; grep error /tmp/pweb_make.log; exit 1; }

# start pweb, capture log
rm -f /tmp/pweb.out
nohup ./pweb pweb.conf > /tmp/pweb.out 2>&1 &
sleep 1

echo "================================================"
echo " PWeb PHP-CGI TEST SUITE"
echo "================================================"
echo
echo "--- 1. static regression ---"
curl -s -o /dev/null -w "index=%{http_code} " "http://127.0.0.1:8080/"
curl -s -o /dev/null -w "about=%{http_code}\n" "http://127.0.0.1:8080/about.html"
echo
echo "--- 2. PHP GET with query params ---"
curl -s --max-time 20 "http://127.0.0.1:8080/cgi-bin/test.php?name=ZCode&lang=C"
echo
echo "--- 3. PHP POST form data ---"
curl -s --max-time 20 -X POST \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "user=admin&action=save&msg=hello" \
  "http://127.0.0.1:8080/cgi-bin/test.php"
echo
echo "--- 4. pweb.out diagnostics ---"
cat /tmp/pweb.out
echo
echo "=== END ==="
