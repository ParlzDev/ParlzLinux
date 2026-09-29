#!/bin/sh
# Direct POST-shim debug: run the php_post_dbg script through the shim with
# a form body on stdin and print what the bootstrap saw.
BODY="user=admin&action=save&msg=hi pweb"
CL="${#BODY}"
echo "shim body len=$CL"
printf '%s' "$BODY" | \
  env \
    GATEWAY_INTERFACE="CGI/1.1" \
    REQUEST_METHOD="POST" \
    SCRIPT_NAME="/php_post_dbg.php" \
    QUERY_STRING="" \
    CONTENT_LENGTH="$CL" \
    CONTENT_TYPE="application/x-www-form-urlencoded" \
    REMOTE_ADDR="127.0.0.1" \
    PWEB_ROOT="/mnt/f/Linux/PWeb/tools" \
    /bin/sh /mnt/f/Linux/PWeb/tools/php-cgi-shim.sh \
      /mnt/f/Linux/PWeb/tools/php_post_dbg.php
echo "=== EXIT=$? ==="
