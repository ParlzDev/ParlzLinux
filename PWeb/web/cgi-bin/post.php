<?php
// PWeb PHP CGI POST test.
// Proves PWeb forwards the request body to the CGI's stdin, and that the
// shim hands it to `php` with variables_order=EGPCS so $_POST is populated.
echo "<!doctype html><html><head><meta charset=utf-8><title>PWeb PHP POST</title></head>\n";
echo "<body><h1>PWeb PHP CGI POST test</h1><pre>\n";
echo "REQUEST_METHOD : " . (getenv("REQUEST_METHOD") ?: "-") . "\n";
echo "CONTENT_LENGTH : " . (getenv("CONTENT_LENGTH") ?: "-") . "\n";
echo "\$_POST (parsed body):\n";
if ($_POST) {
    foreach ($_POST as $k => $v) {
        echo "  $k = " . htmlspecialchars((string)$v) . "\n";
    }
} else {
    echo "  (empty)\n";
}
echo "\nRaw CGI env:\n";
foreach (["REQUEST_METHOD","SCRIPT_NAME","QUERY_STRING","REMOTE_ADDR",
          "SERVER_SOFTWARE","GATEWAY_INTERFACE","CONTENT_LENGTH"] as $e) {
    echo "  $e = " . (getenv($e) ?: "(unset)") . "\n";
}
echo "</pre></body></html>\n";
