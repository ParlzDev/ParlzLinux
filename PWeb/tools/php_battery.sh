#!/bin/bash
# PWeb clean restart + PHP battery runner. Kill all pweb, rebuild, start
# detached (pidfile), then curl the PHP endpoints. Run inside WSL.
set -u
cd /mnt/f/Linux/PWeb

# --- kill all pweb ---
pkill -9 pweb 2>/dev/null
fuser -k 8080/tcp 8099/tcp 2>/dev/null
sleep 1
for p in $(pgrep -x pweb); do kill -9 "$p" 2>/dev/null; done
sleep 0.5
LEFT=$(pgrep -x pweb | wc -l)
echo "pweb still alive after kill: $LEFT"
[ "$LEFT" != "0" ] && { echo "cannot kill pweb, aborting"; exit 1; }

# --- rebuild ---
rm -f main.o conn.o config.o http_parser.o handlers.o util.o mime.o log.o pweb
if ! make >/tmp/build.log 2>&1; then
  echo "BUILD FAILED"; tail -30 /tmp/build.log; exit 1
fi
echo "built $(date -Iseconds) pweb=$(stat -c %y pweb)"

# --- start detached ---
: > /tmp/pweb.out
setsid nohup ./pweb pweb.conf >>/tmp/pweb.out 2>&1 &
echo $! > /tmp/pweb.pid
sleep 2
PID=$(cat /tmp/pweb.pid)
echo "started pweb pid=$PID"
ps -p "$PID" -o pid=,lstart= 2>/dev/null || { echo "pweb not alive"; cat /tmp/pweb.out; exit 1; }

# --- battery ---
B=http://127.0.0.1:8080
echo "===== PWEB PHP BATTERY ====="
echo "--- 1. static regression ---"
curl -s -o /dev/null -w "index=%{http_code} " --max-time 5 $B/
curl -s -o /dev/null -w "about=%{http_code}\n" --max-time 5 $B/about.html
echo "--- 2. PHP GET ?name=ZCode&lang=C ---"
curl -s --max-time 30 "$B/cgi-bin/test.php?name=ZCode&lang=C"
echo "--- 3. PHP POST user=admin&action=save ---"
curl -s --max-time 30 -X POST -H "Content-Type: application/x-www-form-urlencoded" \
  --data "user=admin&action=save&msg=hi pweb" "$B/cgi-bin/test.php"
echo "--- 4. 404 for missing .php ---"
curl -s --max-time 5 -w "\nmissing.php=%{http_code}\n" $B/cgi-bin/does-not-exist.php
echo "--- 5. pweb.out ---"
tail -6 /tmp/pweb.out
echo "===== END ====="
