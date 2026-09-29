#!/usr/bin/env python3
"""真实 OPKG 离线根回归：仅写全新 scratch；不修改宿主机包数据库。"""
import functools
import gzip
import hashlib
import http.server
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import threading

build = Path(os.environ.get('OPKG_BUILD_DIR', '/home/jgzyes/parlz-opkg'))
scratch = Path(tempfile.mkdtemp(prefix='test-', dir=build))
root = scratch / 'root'
feed = scratch / 'feed'
root.mkdir()
feed.mkdir()
for d in ['tmp', 'etc/opkg', 'var/lib/opkg', 'usr/share/opkg/intercept']:
    (root / d).mkdir(parents=True, exist_ok=True)

def tar_bytes(files):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w:gz', format=tarfile.USTAR_FORMAT) as tar:
        for name, (content, mode) in files.items():
            data = content.encode()
            info = tarfile.TarInfo('./' + name)
            info.size, info.mode, info.mtime = len(data), mode, 0
            tar.addfile(info, io.BytesIO(data))
    return out.getvalue()

def package(name, depends='', script=False):
    control = f'Package: {name}\nVersion: 1.0\nArchitecture: all\nMaintainer: Parlz\nDescription: 离线测试\n'
    if depends:
        control += f'Depends: {depends}\n'
    files = {'control': (control, 0o644)}
    if script:
        for phase in ['preinst', 'postinst', 'prerm', 'postrm']:
            files[phase] = (f'#!/bin/sh\nset -eu\nprintf "%s\\n" "{phase}:$1" >> "$PKG_ROOT/scripts.log"\n', 0o755)
    payload = tar_bytes({f'usr/share/{name}/payload': (name + '\n', 0o644)})
    members = [('debian-binary', b'2.0\n'), ('control.tar.gz', tar_bytes(files)), ('data.tar.gz', payload)]
    data = bytearray(b'!<arch>\n')
    for member, body in members:
        header = f'{member + "/":<16}{0:<12}{0:<6}{0:<6}{"100644":<8}{len(body):<10}`\n'
        data.extend(header.encode('ascii'))
        data.extend(body)
        if len(body) % 2:
            data.extend(b'\n')
    filename = name + '_1.0_all.ipk'
    (feed / filename).write_bytes(data)
    return control + f'Filename: {filename}\nSize: {len(data)}\nSHA256sum: {hashlib.sha256(data).hexdigest()}\n\n'

index = package('parlz-test-lib') + package('parlz-test-app', 'parlz-test-lib (>= 1.0)', True)
index += package('parlz-test-broken', 'parlz-missing-dependency')
index += package('parlz-test-checksum')
corrupt = feed / 'parlz-test-checksum_1.0_all.ipk'
corrupt.write_bytes(corrupt.read_bytes()[:-1] + b'X')
(feed / 'Packages').write_text(index)
(feed / 'Packages.gz').write_bytes(gzip.compress(index.encode()))
handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(feed))
server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
conf = root / 'etc/opkg/opkg.conf'
conf.write_text(f'src/gz test http://127.0.0.1:{server.server_port}\ndest root /\narch all 1\narch x86_64 10\noption lists_dir /var/lib/opkg/lists\noption intercepts_dir /usr/share/opkg/intercept\n')
base = ['-f', str(conf), '--offline-root', str(root), '--tmp-dir', str(root / 'tmp')]
log = (scratch / 'commands.log').open('w')

def run(args, success=True, entry='ppm'):
    cmd = [str(build / 'bin' / entry), *base, *args]
    print('+', ' '.join(cmd), flush=True)
    result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='')
    log.write(' '.join(cmd) + '\n' + result.stdout + f'RC={result.returncode}\n')
    log.flush()
    assert (result.returncode == 0) == success, (cmd, result.returncode, result.stdout)
    return result

try:
    version = subprocess.check_output([str(build / 'bin/ppm'), 'opkg', '--version'], text=True)
    assert version.strip() == 'opkg version 0.8.0', version
    print(version, end='')
    run(['update'])
    assert (root / 'var/lib/opkg/lists/test').is_file()
    assert 'arch all 1' in run(['print-architecture']).stdout
    assert 'parlz-test-app' in run(['list']).stdout
    run(['--force-postinstall', 'install', 'parlz-test-app'])
    for name in ['parlz-test-app', 'parlz-test-lib']:
        assert (root / f'usr/share/{name}/payload').read_text() == name + '\n'
    assert 'postinst:configure' in (root / 'scripts.log').read_text()
    assert 'Version: 1.0' in run(['info', 'parlz-test-app']).stdout
    assert 'parlz-test-lib' in run(['list-installed'], entry='opkg').stdout
    run(['install', 'parlz-test-broken'], success=False)
    run(['install', 'parlz-test-checksum'], success=False)
    assert not (root / 'usr/share/parlz-test-checksum/payload').exists()
    run(['remove', 'parlz-test-lib'], success=False)
    run(['--force-postinstall', 'remove', 'parlz-test-app'])
    assert not (root / 'usr/share/parlz-test-app/payload').exists()
    assert 'postrm:remove' in (root / 'scripts.log').read_text()
    run(['remove', 'parlz-test-lib'])
    assert not (root / 'usr/share/parlz-test-lib/payload').exists()
    # 标准本地 .ipk，无 feed 按名称查找的捷径。
    run(['install', str(feed / 'parlz-test-lib_1.0_all.ipk')])
    run(['remove', 'parlz-test-lib'])
    run(['compare-versions', '2.0', '>', '1.0'])
    native = run(['compare-versions', '1.0', '>', '2.0'], success=False, entry='opkg-native')
    delegated = run(['compare-versions', '1.0', '>', '2.0'], success=False)
    assert native.returncode == delegated.returncode
    legacy = subprocess.run([str(build / 'bin/ppm'), 'make'], capture_output=True)
    assert legacy.returncode == 2
    print(f'PASS: update/dependencies/scripts/remove/info/version/config/local-ipk/exit; scratch={scratch}')
finally:
    server.shutdown()
    log.close()
