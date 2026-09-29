#!/usr/bin/env python3
"""本地 HTTPS 测试服务器(供 QEMU guest 经 10.0.2.2 访问)。
用法: e2e-https-server.py <port> <docroot>
自签证书,只用于 --insecure 互操作验证。"""
import http.server
import ssl
import os
import sys

port = int(sys.argv[1])
docroot = sys.argv[2]
os.chdir(docroot)


class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", port), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
# 证书由调用方生成;/tmp/e2e-cert.pem + /tmp/e2e-key.pem
ctx.load_cert_chain("/tmp/e2e-cert.pem", "/tmp/e2e-key.pem")
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
