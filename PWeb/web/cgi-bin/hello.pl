#!/bin/sh
# CGI demo: prints status line + headers + body to stdout.
# QUERY_STRING / REQUEST_METHOD / REMOTE_ADDR come in as env vars set by PWeb.
echo "Content-Type: text/plain"
echo ""
printf 'Hello from CGI!\n'
echo "method   = ${REQUEST_METHOD:-}"
echo "script   = ${SCRIPT_NAME:-}"
echo "query    = ${QUERY_STRING:-}"
echo "remote   = ${REMOTE_ADDR:-}"
echo "host     = ${HTTP_HOST:-}"
