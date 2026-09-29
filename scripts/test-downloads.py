#!/usr/bin/env python3
"""仅 localhost：独立 Python/OpenSSL 服务端及 http.client 响应体 oracle。"""
import collections
import hashlib
import http.client
import pathlib
import socketserver
import ssl
import subprocess
import sys
import threading
import time

work = pathlib.Path(sys.argv[1])
work.mkdir(parents=True, exist_ok=True)
cert, key = work / 'cert.pem', work / 'key.pem'
subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
payload = bytes(range(256)) * 1024 + b'\x00Parlz\r\n'
counts = collections.Counter()
sni = []

class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            request = b''
            self.request.settimeout(5)
            while b'\r\n\r\n' not in request and len(request) < 20000:
                b = self.request.recv(1024)
                if not b:
                    return
                request += b
            path = request.split(b' ')[1].decode()
            counts[(self.server.server_address[1], path)] += 1
            if path == '/slow':
                time.sleep(2)
                return
            if path == '/loop':
                response = b'HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n'
            elif path == '/redirect':
                response = b'HTTP/1.1 302 Found\r\nlocation: /data\r\nContent-Length: 0\r\n\r\n'
            elif path == '/downgrade':
                response = ('HTTP/1.1 302 Found\r\nLocation: http://localhost:%d/data\r\nContent-Length: 0\r\n\r\n' % plain.server_address[1]).encode()
            elif path == '/truncated':
                response = b'HTTP/1.1 200 OK\r\nContent-Length: 999\r\n\r\nshort'
            elif path == '/conflict':
                response = b'HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx'
            elif path == '/badchunk':
                response = b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\nx\r\n0\r\n\r\n'
            elif path == '/missing':
                response = b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n'
            elif path == '/chunked':
                response = b'HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\ntRaNsFeR-EnCoDiNg: Chunked\r\n\r\n'
                for i in range(0, len(payload), 777):
                    part = payload[i:i+777]
                    response += ('%x;test=yes\r\n' % len(part)).encode() + part + b'\r\n'
                response += b'0\r\nX-Checksum: checked\r\n\r\n'
            else:
                response = ('HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n' % len(payload)).encode() + payload
            # 刻意跨行/跨 chunk 大小/CRLF 边界分片；正文大于读取缓冲。
            for i in range(min(150, len(response))):
                self.request.sendall(response[i:i+1])
            for i in range(150, len(response), 137):
                self.request.sendall(response[i:i+137])
            if path == '/data':
                time.sleep(.3)
        except (OSError, IndexError):
            pass

class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
    def get_request(self):
        sock, addr = super().get_request()
        if getattr(self, 'tls_context', None):
            sock.settimeout(5)
            try:
                sock = self.tls_context.wrap_socket(sock, server_side=True)
            except Exception:
                sock.close()
                raise
        return sock, addr

plain = Server(('127.0.0.1', 0), Handler)
secure = Server(('127.0.0.1', 0), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
ctx.set_servername_callback(lambda sock, name, context: sni.append(name))
secure.tls_context = ctx
for server in (plain, secure):
    threading.Thread(target=server.serve_forever, daemon=True).start()

failures = []
checks = 0
def check(label, condition, detail=''):
    global checks
    checks += 1
    print(('PASS ' if condition else 'FAIL ') + label + (' ' + detail if not condition else ''), flush=True)
    if not condition:
        failures.append(label)

def run(tool, path, tls=False, extra=(), timeout=8):
    port = (secure if tls else plain).server_address[1]
    url = ('https' if tls else 'http') + '://localhost:%d' % port + path
    args = [str(work/'bin'/tool)]
    if tool == 'wget':
        args += ['-O', '-']
    args += list(extra) + [url]
    return subprocess.run(args, capture_output=True, timeout=timeout)

try:
    for tls in (False, True):
        port = (secure if tls else plain).server_address[1]
        for path in ('/data', '/chunked'):
            if tls:
                conn = http.client.HTTPSConnection('localhost', port, context=ssl.create_default_context(cafile=str(cert)))
            else:
                conn = http.client.HTTPConnection('localhost', port)
            conn.request('GET', path)
            oracle = conn.getresponse().read()
            conn.close()
            check('oracle ' + str(tls) + path, oracle == payload)
            for tool in ('curl', 'wget'):
                before = counts[(port, path)]
                p = run(tool, path, tls, ['--insecure'] if tls else [])
                check(tool + ' ' + str(tls) + path, p.returncode == 0 and p.stdout == oracle,
                      str(p.returncode) + ' ' + p.stderr.decode(errors='replace')[:250])
                check(tool + ' single GET ' + str(tls) + path, counts[(port, path)] == before+1)
    # OpenSSL 后端按服务器能力协商 TLS 1.2/1.3,不再人为禁用 1.2。
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    for tool in ('curl', 'wget'):
        p = run(tool, '/data', True, ['--insecure'])
        check(tool+' TLS 1.2 negotiates with --insecure', p.returncode == 0 and p.stdout == payload,
              str(p.returncode)+' '+p.stderr.decode(errors='replace')[:250])
    # 严格模式在仅 1.2 的服务器上也应成功(证书可信,由 oracle 证明)。
    strict12 = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    strict12.maximum_version = ssl.TLSVersion.TLSv1_2
    strict12.load_verify_locations(str(cert))
    c12 = http.client.HTTPSConnection('localhost', secure.server_address[1], context=strict12)
    c12.request('GET', '/data')
    ok12 = c12.getresponse().read() == payload
    c12.close()
    check('oracle strict TLS 1.2', ok12)
    ctx.maximum_version = ssl.TLSVersion.MAXIMUM_SUPPORTED
    for tool in ('curl', 'wget'):
        for path, code in [('/truncated',18),('/conflict',18),('/badchunk',18),('/loop',47)]:
            p = run(tool, path, extra=['-L'])
            check(tool+path, p.returncode == code)
        p = run(tool, '/redirect', extra=['-L'])
        check(tool+' redirect', p.returncode == 0 and p.stdout == payload)
        p = run(tool, '/missing', extra=['-f'])
        check(tool+' HTTP failure', p.returncode == 22 and p.stdout == b'')
        t = time.monotonic()
        p = run(tool, '/slow', extra=['-T' if tool == 'wget' else '-m', '1'])
        check(tool+' total timeout', p.returncode == 28 and time.monotonic()-t < 1.8)
        p = run(tool, '/data', True)
        check(tool+' untrusted cert fail closed', p.returncode == 60 and not p.stdout)
        # OpenSSL 后端 --cacert 是真校验:提供的 CA 能验证测试证书,应成功。
        p = run(tool, '/data', True, ['--cacert', str(cert)])
        check(tool+' cacert strict verify succeeds', p.returncode == 0 and p.stdout == payload,
              str(p.returncode)+' '+p.stderr.decode(errors='replace')[:250])
        p = run(tool, '/data', True, ['--cacert', '/nonexistent-ca.pem'])
        check(tool+' missing cacert fails closed', p.returncode == 58 and not p.stdout)
        p = run(tool, '/downgrade', True, ['--insecure', '-L'])
        check(tool+' no HTTPS downgrade', p.returncode == 60 and not p.stdout)
        target = work/(tool+'.download')
        target.write_bytes(b'original')
        p = run(tool, '/truncated', extra=['-O' if tool == 'wget' else '-o', str(target)])
        check(tool+' failed download preserves file', p.returncode == 18 and target.read_bytes() == b'original')
    # 进度条:非静默时 stderr 含进度标记,wget 含百分比,curl 含 # 或 .
    for tool in ('curl', 'wget'):
        p = run(tool, '/data')
        check(tool+' progress bar in stderr',
              (b'%' in p.stderr) or (b'#' in p.stderr),
              p.stderr.decode(errors='replace')[:80])
    # -q / --no-progress 关进度
    p = run('wget', '/data', extra=['-q'])
    check('wget -q no progress bar', b'%' not in p.stderr and b'#' not in p.stderr,
          p.stderr.decode(errors='replace')[:80])
    p = run('curl', '/data', extra=['-s'])
    check('curl -s no progress bar', b'%' not in p.stderr and b'#' not in p.stderr,
          p.stderr.decode(errors='replace')[:80])
    check('SNI localhost observed', 'localhost' in sni)
    p = subprocess.run([str(work/'bin'/'curl-no-tls'), '--insecure',
                        'https://localhost:%d/data' % secure.server_address[1]], capture_output=True, timeout=5)
    check('no TLS build rejects HTTPS', p.returncode == 35 and not p.stdout)
finally:
    plain.shutdown()
    secure.shutdown()
print('payload sha256=' + hashlib.sha256(payload).hexdigest())
print('%d checks; %d failures; OpenSSL=%s' % (checks, len(failures), ssl.OPENSSL_VERSION))
sys.exit(bool(failures))
