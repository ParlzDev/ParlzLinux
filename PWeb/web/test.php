<?php
header('Content-Type: text/html; charset=utf-8');
echo "<h1>PHP works</h1>";
echo "<p>PHP version: " . phpversion() . "</p>";
echo "<p>Query: " . htmlspecialchars($_GET['q'] ?? '(none)') . "</p>";
?>
