#!/usr/bin/env python3
# login 流 E2E: host pty 认证状态机(快) + QEMU sysinit 自动首登交互(慢)。
# 用法: wsl -d Ubuntu-26.04 -u root -e python3 /mnt/f/Linux/Parlz/scripts/login-reactive.py
# 前提: images/parlz-bzImage 是 test-login.sh [2/3] 嵌好的最新内核
#       (inittab sysinit 自动触发 body, 无需按键)。
import os, sys, re, time, subprocess, threading, shutil, pty, select

IMG  = "/mnt/f/Linux/Parlz/images"
DISK = "/tmp/parlz-final17.img"
LOGIN = "/home/jgzyes/parlz-userland/build/bin/login"
HOST_AUTH = "/etc/parlz-auth"

# ============ [1] host pty 认证状态机 ============
print("=== [1] host pty 认证状态机 ===")
assert os.path.exists(LOGIN), f"缺 {LOGIN}(先跑 test-login.sh 重编 login)"

def drain(fd, ms):
    total = b""
    end = time.time() + ms/1000
    while time.time() < end:
        r,_,_ = select.select([fd],[],[],0.1)
        if fd in r:
            try: c = os.read(fd, 4096)
            except OSError: break
            if not c: break
            total += c
            end = time.time() + 0.3
    return total

def pty_test(name, auth_content, steps, expect_reject):
    BAK = HOST_AUTH + ".bak"
    if os.path.exists(HOST_AUTH) and not os.path.exists(BAK):
        shutil.copy2(HOST_AUTH, BAK)
    if auth_content is None:
        if os.path.exists(HOST_AUTH): os.remove(HOST_AUTH)
    else:
        with open(HOST_AUTH, "w") as f: f.write(auth_content)
        os.chmod(HOST_AUTH, 0o600)
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(LOGIN, [LOGIN])
    out = b""
    for i, (data, wait) in enumerate(steps):
        time.sleep(0.3)
        if data:
            try: os.write(fd, data)
            except OSError: break
        out += drain(fd, 300 if i < len(steps)-1 else 1500)
    time.sleep(0.3)
    try: os.waitpid(pid, 0)
    except: pass
    txt = out.decode("utf-8", "replace")
    ok = (expect_reject and "拒绝" in txt) or \
         (not expect_reject and "拒绝" not in txt)
    print(f"  {name} {'PASS' if ok else 'FAIL'}")
    if not ok:
        print(f"    输出尾: {txt[-300:]!r}")
    return ok

BAK = HOST_AUTH + ".bak"
try:
    r1 = pty_test("A1 正确凭据", "tester\nparlz\n",
                  [(b"tester\n",0.4),(b"parlz\n",1.2)], False)
    r2 = pty_test("A2 错误密码", "tester\nparlz\n",
                  [(b"tester\n",0.4),(b"wrong\n",0.4),
                   (b"tester\n",0.4),(b"wrong\n",0.4),
                   (b"tester\n",0.4),(b"wrong\n",1.2)], True)
    r3 = pty_test("B  首登建凭证", None,
                  [(b"newuser\n",0.4),(b"pw123\n",0.4),(b"pw123\n",1.2)], False)
    if os.path.exists(HOST_AUTH):
        r3 = r3 and ("newuser" in open(HOST_AUTH).read())
    r4 = pty_test("B2 新凭证登录", None,
                  [(b"newuser\n",0.4),(b"pw123\n",1.2)], False)
finally:
    if os.path.exists(BAK):
        shutil.move(BAK, HOST_AUTH)
    elif os.path.exists(HOST_AUTH):
        os.remove(HOST_AUTH)
pty_ok = r1 and r2 and r3 and r4
print(f"  host pty: {'ALL_OK' if pty_ok else 'FAIL'}")

# ============ [2] QEMU sysinit 自动首登交互 ============
print("\n=== [2] QEMU sysinit 自动首登交互 ===")
os.system(f"rm -f {DISK} /tmp/lgr.log")
open(DISK, "wb").close()
open(DISK, "r+b").truncate(256*1024*1024)

# 注入序列: sysinit 自动跑 body(安装+网络约 35s 到 login),
# login 弹 Username: → 注入 tester/pw123/pw123, 之后 whoami 验证。
# 用 (sleep; printf ...) | qemu -serial stdio 模式(已验证可用)。
inject = """
set -e
sleep 38
printf 'tester\\n'
sleep 4
printf 'pw123\\n'
sleep 4
printf 'pw123\\n'
sleep 6
printf 'whoami\\n'
sleep 5
printf 'exit\\n'
"""
cmd = (f"bash -c '{inject}' | timeout 110 /usr/bin/qemu-system-x86_64 -m 512M -nographic -no-reboot "
       f"-kernel {IMG}/parlz-bzImage -append 'console=ttyS0,115200' "
       f"-drive file={DISK},if=virtio,format=raw "
       f"-serial stdio -monitor none 2>/dev/null > /tmp/lgr.log")
subprocess.run(cmd, shell=True, executable="/bin/bash")

cap = open("/tmp/lgr.log","rb").read().decode("utf-8","replace")
u2s = "=== Parlz user-space ===" in cap
b2s = "boot ready" in cap
auth1 = ("已创建用户 tester" in cap) or ("认证通过" in cap)
nonet = "root@(none)" not in cap
wam = False
if "whoami" in cap:
    seg = cap[cap.find("whoami"):cap.find("whoami")+120]
    wam = "tester" in seg
panic = "Kernel panic" in cap
print(f"  busybox-init user-space={u2s}  boot ready={b2s}")
print(f"  首登建凭证={auth1}  无 root@(none)={nonet}  whoami→tester={wam}  panic={panic}")
if panic:
    print("  --- panic 尾 15 ---")
    for ln in cap.splitlines()[-15:]: print("  ", ln)
qemu_ok = u2s and b2s and auth1 and nonet and wam and not panic

print("\n=== 总判定 ===")
print(f"  HOST_PTY={'OK' if pty_ok else 'FAIL'}  QEMU_E2E={'OK' if qemu_ok else 'FAIL'}")
sys.exit(0 if (pty_ok and qemu_ok) else 1)
