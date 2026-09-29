#!/bin/sh
# Direct shim POST test (bypasses pweb; verifies shim+php handle a body).
cd /mnt/f/Linux/PWeb/web 2>/dev/null || true
BODY="user=admin&action=save&msg=hi pweb"
# Simulate exactly what PWeb does: CGI env + body piped to shim stdin.
printf '%s' "$BODY" | \
  env \
    GATEWAY_INTERFACE="CGI/1.1" \
    REQUEST_METHOD="POST" \
    SCRIPT_NAME="/cgi-bin/test.php" \
    QUERY_STRING="" \
    CONTENT_LENGTH="${#BODY}" \
    CONTENT_TYPE="application/x-www-form-urlencoded" \
    REMOTE_ADDR="127.0.0.1" \
    PWEB_ROOT="/mnt/f/Linux/PWeb/web" \
    /bin/sh /mnt/f/Linux/PWeb/tools/php-cgi-shim.sh \
      /mnt/f/Linux/PWeb/web/cgi-bin/test.php
echo "=== POST-SHIM-EXIT=$? ==="
