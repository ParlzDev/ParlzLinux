<?php
// PWeb PHP feature test battery. Each section exercises one PHP + CGI
// capability so the curl output can be grepped per-feature.

// --- 4. Session (start before any output so headers stay clean) ----------
ini_set("session.save_path", "/tmp");
session_start();
$_SESSION['hits'] = (int)($_SESSION['hits'] ?? 0) + 1;

header("Content-Type: text/plain");

// --- 1. PHP engine -------------------------------------------------------
echo "PHP_VERSION=", phpversion(), "\n";
echo "SAPI=", php_sapi_name(), "\n";
echo "OS=", PHP_OS_FAMILY, "\n";

// --- 2. CGI env PWeb sets up --------------------------------------------
$env = ["REQUEST_METHOD","SCRIPT_NAME","QUERY_STRING","REMOTE_ADDR",
        "SERVER_SOFTWARE","GATEWAY_INTERFACE","CONTENT_LENGTH","PWEB_ROOT"];
foreach ($env as $k) {
    $v = $_SERVER[$k] ?? null;
    echo "  ", $k, "=", ($v !== null && $v !== "" ? $v : "(unset)"), "\n";
}

// --- 3. $_GET / $_POST ---------------------------------------------------
if ($_GET) {
    echo "GET=";
    foreach ($_GET as $k => $v) { echo $k, "=", $v, " "; }
    echo "\n";
}
if ($_POST) {
    echo "POST=";
    foreach ($_POST as $k => $v) { echo $k, "=", $v, " "; }
    echo "\n";
}
if (!$_GET && !$_POST) {
    echo "NO_PARAMS\n";
}

// --- session counter -----------------------------------------------------
echo "SESSION_HITS=", $_SESSION['hits'], "\n";

// --- 5. Include ----------------------------------------------------------
$lib = dirname(__FILE__) . "/lib.php";
if (file_exists($lib)) {
    require_once $lib;
    echo "INCLUDE=", pweb_helper(), "\n";
}

// --- 6. Error handling ---------------------------------------------------
// Exercise a recoverable error and confirm PHP survives it.
$triggered = false;
set_error_handler(function ($no, $msg) use (&$triggered) {
    $triggered = true;
    return true; // handle it, don't escalate
});
trigger_error("pweb-simulated-warning", E_USER_WARNING);
restore_error_handler();
echo "ERROR_HANDLED=", $triggered ? "yes" : "no", "\n";

$err = error_get_last();
echo "LAST_ERROR=", $err ? $err['message'] : "none", "\n";
session_write_close();
