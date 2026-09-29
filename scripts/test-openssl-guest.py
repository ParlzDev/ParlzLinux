#!/usr/bin/env python3
"""隔离 initramfs 覆盖层；仅本地 QEMU 与临时证书，不改生产 rootfs。"""
import http.server
import pathlib
import shutil
import ssl
import subprocess
import tempfile
import threading

repo = pathlib.Path('/mnt/f/Linux/Parlz')
openssl = pathlib.Path('/home/jgzyes/parlz-openssl/stage/bin/openssl')
work = pathlib.Path(tempfile.mkdtemp(prefix='parlz-openssl-guest-'))
overlay = work / 'overlay'
(overlay / 'bin').mkdir(parents=True)
shutil.copy2(openssl, overlay / 'bin/openssl')
cert, key = work / 'cert.pem', work / 'key.pem'
subprocess.run(['/usr/bin/openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                '-days', '1', '-subj', '/CN=localhost', '-addext',
                'subjectAltName=DNS:localhost,IP:10.0.2.2', '-keyout', str(key),
                '-out', str(cert)], check=True, capture_output=True)
shutil.copy2(cert, overlay / 'probe-ca.pem')
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'OPENSSL_GUEST_BODY\n'
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, *args):
        pass
servers = []
for version in (ssl.TLSVersion.TLSv1_3, ssl.TLSVersion.TLSv1_2):
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = context.maximum_version = version
    context.load_cert_chain(cert, key)
    server.socket = context.wrap_socket(server.socket, server_side=True)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    servers.append(server)
p13, p12 = [s.server_port for s in servers]
script = f'''#!/bin/sh
ifc auto 10.0.2.15 255.255.255.0 10.0.2.2
openssl version
printf 'GET / HTTP/1.0\\r\\nHost: localhost\\r\\n\\r\\n' > /tmp/probe.req
probe() {{
  # $1=标记 $2=额外参数; 服务器不发 close_notify,忽略 -quiet 的 EOF 差异,
  # 成功与否只看响应体。用 case 而不是 grep(旧版 initramfs 的 grep 有死循环 bug)。
  name=$1; shift
  openssl s_client "$@" -quiet < /tmp/probe.req > /tmp/p.out 2>/tmp/p.err
  case "$(cat /tmp/p.out)" in
    *OPENSSL_GUEST_BODY*) echo "$name" ;;
  esac
}}
probe PROBE_TLS13_OK -connect 10.0.2.2:{p13} -verify_ip 10.0.2.2 -verify_return_error -CAfile /probe-ca.pem -tls1_3
probe PROBE_TLS12_OK -connect 10.0.2.2:{p12} -verify_ip 10.0.2.2 -verify_return_error -CAfile /probe-ca.pem -tls1_2
openssl s_client -connect 10.0.2.2:{p13} -verify_ip 10.0.2.2 -verify_return_error -quiet < /tmp/probe.req > /tmp/untrusted.out 2>/tmp/untrusted.err
case "$(cat /tmp/untrusted.err)" in
  *certificate\ verify\ failed*) echo PROBE_UNTRUSTED_REJECT ;;
esac
openssl s_client -connect 10.0.2.2:{p13} -verify_hostname wrong.invalid -verify_return_error -CAfile /probe-ca.pem -quiet < /tmp/probe.req > /tmp/host.out 2>/tmp/host.err
case "$(cat /tmp/host.err)" in
  *hostname\ mismatch*) echo PROBE_HOSTNAME_REJECT ;;
esac
echo PROBE_FINISHED
'''
(overlay / 'nettest.sh').write_text(script)
(overlay / 'nettest.sh').chmod(0o755)
image = work / 'probe-initramfs.gz'
shutil.copyfile(repo / 'images/parlz-initramfs', image)
with image.open('ab') as out:
    subprocess.run(['sh', '-c', 'find . -print0 | cpio --null -o -H newc | gzip -1'],
                   cwd=overlay, stdout=out, check=True)
log = repo / 'images/openssl-probe.log'
cmd = ['qemu-system-x86_64', '-m', '512M', '-display', 'none', '-monitor', 'none',
       '-serial', 'file:' + str(log), '-no-reboot', '-kernel', str(repo/'images/parlz-bzImage'),
       '-append', 'console=ttyS0,115200 earlycon', '-initrd', str(image),
       '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0']
try:
    import time
    with subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as proc:
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline and proc.poll() is None:
            time.sleep(1)
            if log.exists() and 'PROBE_FINISHED' in log.read_text(errors='replace'):
                break
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    text = log.read_text(errors='replace')
    markers = ['PROBE_TLS13_OK', 'PROBE_TLS12_OK', 'PROBE_UNTRUSTED_REJECT', 'PROBE_HOSTNAME_REJECT']
    for marker in markers:
        print(('PASS ' if marker in text else 'FAIL ') + marker)
    print('日志:', log, '隔离工作目录:', work)
    if not all(marker in text for marker in markers):
        print(text[-14000:])
        raise SystemExit(1)
finally:
    for server in servers:
        server.shutdown()
