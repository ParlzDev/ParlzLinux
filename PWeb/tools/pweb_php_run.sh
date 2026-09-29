#!/bin/bash
# PWeb PHP-CGI test runner: run inside WSL, drives the PHP test battery.
set -u
B=http://127.0.0.1:8080
for p in $(ps -eo pid,args | grep "pweb pweb.conf" | grep -v grep | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
done
sleep 0.5
cd /mnt/f/Linux/PWeb
nohup ./pweb pweb.conf > /tmp/pweb.out 2>&1 &
sleep 1
echo "=== pweb status ==="
cat /tmp/pweb.out
echo ""
echo "===== PHP TEST BATTERY ====="
echo "--- 1. GET: /cgi-bin/test.php?name=ZCode&lang=C ---"
curl -s --max-time 15 "$B/cgi-bin/test.php?name=ZCode&lang=C"
echo ""
echo "--- 2. POST: /cgi-bin/test.php (form data) ---"
curl -s --max-time 15 -X POST -H "Content-Type: application/x-www-form-urlencoded" \
  --data "user=admin&action=save&msg=hello pweb" "$B/cgi-bin/test.php"
echo ""
echo "--- 3. static regression: index + about ---"
curl -s -o /dev/null -w "index=%{http_code} " --max-time 5 "$B/"
curl -s -o /dev/null -w "about=%{http_code}\n" --max-time 5 "$B/about.html"
echo "--- 4. session counter (2nd GET should show hits=2) ---"
curl -s --max-time 15 -c /tmp/pweb_cookie.txt "$B/cgi-bin/test.php?n=2" | grep SESSION_HITS
curl -s --max-time 15 -b /tmp/pweb_cookie.txt "$B/cgi-bin/test.php?n=2" | grep SESSION_HITS
