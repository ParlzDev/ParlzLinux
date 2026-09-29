#!/bin/bash
# PWeb PHP-CGI full test suite (run inside WSL).
set -u
cd /mnt/f/Linux/PWeb
# kill all pweb
for p in $(ps -C pweb -o pid= 2>/dev/null); do kill -9 "$p" 2>/dev/null; done
fuser -k 8080/tcp 8099/tcp 2>/dev/null
sleep 1
# clean rebuild
rm -f main.o conn.o config.o http_parser.o handlers.o util.o mime.o log.o pweb
make >/dev/null 2>&1
[ -x ./pweb ] || { echo "BUILD FAILED"; exit 1; }
# start pweb
rm -f /tmp/pweb.out
nohup ./pweb pweb.conf >/tmp/pweb.out 2>&1 &
sleep 1
B=http://127.0.0.1:8080
echo "================================================"
echo " PWeb PHP-CGI TEST SUITE"
echo "================================================"
echo
echo "--- 1. static regression ---"
curl -s -o /dev/null -w "index=%{http_code} " "$B/"
curl -s -o /dev/null -w "about=%{http_code}\n" "$B/about.html"
echo
echo "--- 2. PHP GET with query: /cgi-bin/test.php?name=ZCode&lang=C ---"
curl -s --max-time 20 "$B/cgi-bin/test.php?name=ZCode&lang=C"
echo
echo "--- 3. PHP POST form: /cgi-bin/test.php ---"
curl -s --max-time 20 -X POST \
  -H "Content-Type: application/x-www-form-urlencoded" \
  --data "user=admin&action=save&msg=hi pweb" \
  "$B/cgi-bin/test.php"
echo
echo "--- 4. 404 for missing .php ---"
curl -s -i --max-time 5 "$B/cgi-bin/does-not-exist.php" | head -3
echo
echo "--- 5. pweb.out diagnostics ---"
cat /tmp/pweb.out
echo "================================================"
echo " END"
echo "================================================"
