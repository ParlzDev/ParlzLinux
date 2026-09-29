<?php
// PWeb CGI test page. Exposes CGI env vars PWeb sets (GATEWAY_INTERFACE,
// REQUEST_METHOD, SCRIPT_NAME, QUERY_STRING, REMOTE_ADDR, SERVER_SOFTWARE)
// so we can verify the CGI plumbing end-to-end under WSL.
echo "<!doctype html><html><head><meta charset=utf-8><title>PWeb PHP CGI</title></head>\n";
echo "<body><h1>PWeb PHP CGI test</h1><pre>\n";
echo "PHP version : " . phpversion() . "\n";
echo "REQUEST_METHOD : " . (getenv("REQUEST_METHOD") ?: "-") . "\n";
echo "SCRIPT_NAME : " . (getenv("SCRIPT_NAME") ?: "-") . "\n";
echo "QUERY_STRING : " . (getenv("QUERY_STRING") ?: "-") . "\n";
echo "REMOTE_ADDR : " . (getenv("REMOTE_ADDR") ?: "-") . "\n";
echo "SERVER_SOFTWARE : " . (getenv("SERVER_SOFTWARE") ?: "-") . "\n";
echo "GATEWAY_INTERFACE : " . (getenv("GATEWAY_INTERFACE") ?: "-") . "\n";
echo "Server time : " . date("Y-m-d H:i:s") . "\n";
// echo back $_GET so ?a=b&c=d style queries are visible
echo "\n\$_GET (parsed query):\n";
foreach ($_GET as $k => $v) {
    echo "  $k = " . htmlspecialchars((string)$v) . "\n";
}
echo "</pre></body></html>\n";
