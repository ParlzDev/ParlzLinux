#!/usr/bin/env python3
"""QEMU 端到端:guest curl/wget(OpenSSL 后端)严格 HTTPS + 既有验收回归。
- 宿主起 HTTP(8080) + HTTPS(8443,CA 签发 SAN=IP:10.0.2.2 的服务器证书)
- CA 证书经 overlay cpio 注入 /probe-ca.pem
- 检查项:严格 HTTPS 成功、错误 CA 失败关闭、HTTP 明文、wget、bash/chmod/opkg 回归
"""
import http.server
import pathlib
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time

repo = pathlib.Path('/mnt/f/Linux/Parlz')
img = repo / 'images'
work = pathlib.Path(tempfile.mkdtemp(prefix='parlz-e2e-openssl-'))
overlay = work / 'overlay'
(overlay / 'etc').mkdir(parents=True)

# 1) 服务器证书(私有 CA 签发,SAN 含 10.0.2.2)与 CA
ca_key, ca_cert = work / 'ca.key', work / 'ca.pem'
srv_key, srv_csr, srv_cert = work / 'srv.key', work / 'srv.csr', work / 'srv.pem'
def run_ssl(*args):
    subprocess.run(['/usr/bin/openssl', *args], check=True, capture_output=True)
run_ssl('req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
        '-subj', '/CN=Parlz E2E CA', '-keyout', str(ca_key), '-out', str(ca_cert))
run_ssl('req', '-newkey', 'rsa:2048', '-nodes', '-subj', '/CN=10.0.2.2',
        '-keyout', str(srv_key), '-out', str(srv_csr))
# 用 CA 签发服务器证书(补 IP SAN;guest 用数字 IP 连接,校验按 iPAddress 匹配)
ext = work / 'san.cnf'
ext.write_text('subjectAltName=IP:10.0.2.2\n')
run_ssl('x509', '-req', '-in', str(srv_csr), '-CA', str(ca_cert), '-CAkey', str(ca_key),
        '-CAcreateserial', '-days', '1', '-extfile', str(ext), '-out', str(srv_cert))
(overlay / 'probe-ca.pem').write_bytes(ca_cert.read_bytes())
# 系统信任库(真实公网 CA) + 测试 CA,覆盖 initramfs 内的 /etc/ssl/cert.pem:
# STRICT_SYS_OK 用例由此走 SSL_CTX_set_default_verify_paths() 真实路径。
bundle = pathlib.Path('/etc/ssl/certs/ca-certificates.crt').read_bytes()
(overlay / 'etc' / 'ssl').mkdir(parents=True, exist_ok=True)
(overlay / 'etc' / 'ssl' / 'cert.pem').write_bytes(bundle + ca_cert.read_bytes())

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'PARLZ_OPENSSL_E2E_BODY\n'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass

docroot = work / 'docroot'
docroot.mkdir()
(docroot / 'hello.txt').write_bytes(b'PARLZ_HTTP_BODY\n')

https_server = http.server.ThreadingHTTPServer(('127.0.0.1', 8443), Handler)
hctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
hctx.load_cert_chain(srv_cert, srv_key)
https_server.socket = hctx.wrap_socket(https_server.socket, server_side=True)
threading.Thread(target=https_server.serve_forever, daemon=True).start()
http_server = http.server.ThreadingHTTPServer(('127.0.0.1', 8080), Handler)
threading.Thread(target=http_server.serve_forever, daemon=True).start()

log = img / 'parlz-e2e.log'
image = work / 'e2e-initramfs.gz'
shutil.copyfile(img / 'parlz-initramfs', image)
with image.open('ab') as out:
    subprocess.run(['sh', '-c', 'find . -print0 | cpio --null -o -H newc | gzip -1'],
                   cwd=overlay, stdout=out, check=True)

try:
    with subprocess.Popen(
            ['qemu-system-x86_64', '-m', '512M', '-display', 'none', '-monitor', 'none',
             '-serial', 'file:' + str(log), '-no-reboot',
             '-kernel', str(img / 'parlz-bzImage'),
             '-append', 'console=ttyS0,115200 earlycon',
             '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
             '-initrd', str(image)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as proc:
        deadline = time.monotonic() + 180
        while time.monotonic() < deadline and proc.poll() is None:
            time.sleep(2)
            if log.exists() and 'E2E_FINISHED' in log.read_text(errors='replace'):
                break
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
finally:
    https_server.shutdown()
    http_server.shutdown()

text = log.read_text(errors='replace') if log.exists() else ''
fail = 0
def check(label, marker):
    global fail
    ok = marker in text
    print(('PASS ' if ok else 'FAIL ') + label, flush=True)
    if not ok:
        fail += 1

check('内核启动进入用户空间', '=== Parlz user-space ===')
check('curl 严格 HTTPS 系统信任库校验通过', 'STRICT_SYS_OK')
check('curl --cacert 严格 HTTPS 校验通过', 'STRICT_CACERT_OK')
check('curl 假 CA 失败关闭', 'STRICT_BADCA_REJECT')
check('curl --insecure TLS 可用(对照)', 'INSECURE_OK')
check('curl HTTP 明文', 'HTTP_OK')
check('wget HTTPS --ca-certificate= 等号形式', 'WGET_CACERT_OK')
check('wget HTTP', 'WGET_OK')
check('grep 文件参数(修复后,wc 计数 bug 除外)', 'GREP_FILE_FAIL' not in text and 'GREP_FILE_OK' in text)
check('opkg 版本', 'opkg version 0.8.0')
print('=== RESULT: fail=%d ===' % fail)
if fail:
    print(text[-6000:])
    sys.exit(1)
