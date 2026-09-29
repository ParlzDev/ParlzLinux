<?php
// Debug: dump what the shim bootstrap actually put into $_POST / raw stdin.
echo "POST_KEYS=";
foreach ($_POST as $k => $v) { echo $k, "=", $v, " "; }
echo "\nPOST_EMPTY=", var_export(empty($_POST), true), "\n";
$raw = @stream_get_contents(STDIN);
echo "STDIN_AFTER=", var_export($raw, true), "\n";
echo "CL=", getenv("CONTENT_LENGTH"), "\n";
echo "CT=", (getenv("CONTENT_TYPE") ?: "(unset)"), "\n";
