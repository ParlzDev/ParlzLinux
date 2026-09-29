#!/bin/sh
# 验证 sh 交互回显/换行: 喂入若干行命令, 检查输出流里提示符前都有换行
set -e
cd /home/jgzyes/parlz-userland
mkdir -p /tmp/shfix
cat > /tmp/shfix/test.sh <<'EOF'
echo hello
pwd
ls bin | head
EOF
# 用 pty 驱动交互模式(模拟真实终端): 提示符与输出交替, 检查粘行
python3 - <<'PYEOF'
import os, pty, time, fcntl, termios, struct, re

pid, fd = pty.fork()
if pid == 0:
    os.chdir("/home/jgzyes/parlz-userland")
    env = {"HOME": "/home/jgzyes/parlz-userland",
           "LS_COLORS": "1"}
    os.execve("./build/bin/sh", ["sh"], env)

out = b""
try:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
except OSError:
    pass

deadline = time.time() + 25
sent = False
while time.time() < deadline:
    try:
        data = os.read(fd, 4096)
    except OSError:
        break
    if not data:
        break
    out += data
    if not sent and b"Parlz shell" in out:
        os.write(fd, b"echo hello\n")
        time.sleep(0.3)
        os.write(fd, b"pwd\n")
        time.sleep(0.3)
        os.write(fd, b"echo world\n")
        time.sleep(0.3)
        os.write(fd, b"exit\n")
        sent = True
    time.sleep(0.05)

try:
    os.close(fd)
except OSError:
    pass
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass

text = out.decode("utf-8", "replace")
text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)  # 去 ANSI
print("=== captured ===")
print(text)
print("=== checks ===")
lines = text.splitlines()
# 关键检查: 每个 "parlz@..." 提示符必须独占一行开头(前面是 \n), 不允许
# 出现 "...> parlz@(none)" 这种粘行
joined = "\n".join(lines)
bad = [l for l in lines if "> parlz@" in l]
if bad:
    print("FAIL: 提示符粘行:", bad[:3])
else:
    print("OK: 无粘行")
if "hello" in text and "world" in text:
    print("OK: echo 正常输出")
else:
    print("FAIL: echo 输出缺失")
if "bye" in text:
    print("OK: exit 正常退出")

# /bin/bash 兼容入口(软链到 sh): 脚本模式与无环境交互模式
import subprocess
r = subprocess.run(["./root/bin/bash", "-c", "echo BASH-OK"],
                   cwd="/home/jgzyes/parlz-userland",
                   capture_output=True, text=True)
if "BASH-OK" in r.stdout:
    print("OK: /bin/bash 脚本模式可用")
else:
    print("FAIL: /bin/bash 脚本模式:", r.stdout, r.stderr)
# -c 带分号多句 + 引号内容
r3 = subprocess.run(["./root/bin/bash", "-c",
                    "echo A; echo \"hello world\"; pwd"],
                   cwd="/home/jgzyes/parlz-userland",
                   capture_output=True, text=True)
if "A" in r3.stdout and "hello world" in r3.stdout and "/home/jgzyes/parlz-userland" in r3.stdout:
    print("OK: sh -c 多句/引号解析")
else:
    print("FAIL: sh -c 多句:", r3.stdout, r3.stderr)
r2 = subprocess.run(["./build/bin/sh"], input="echo envless-ok\nexit\n",
                    cwd="/home/jgzyes/parlz-userland",
                    env={}, capture_output=True, text=True, timeout=10)
if "envless-ok" in r2.stdout:
    print("OK: 无环境启动 sh(模拟 init 场景)")
else:
    print("FAIL: 无环境启动:", r2.stdout[:200], r2.stderr[:200])

# 提示符风格: 无 tty 启动(stdin EOF), 比较 sh/bash 提示符标记
import subprocess as sp2
rb = sp2.run(["./root/bin/bash"], stdin=sp2.DEVNULL,
             capture_output=True, text=True, timeout=10,
             cwd="/home/jgzyes/parlz-userland")
rs = sp2.run(["./root/bin/sh"], stdin=sp2.DEVNULL,
             capture_output=True, text=True, timeout=10,
             cwd="/home/jgzyes/parlz-userland")
bl = [l for l in rb.stdout.splitlines() if "@" in l and ("$" in l or "#" in l)]
if bl:
    print("OK: /bin/bash 提示符为 bash 风格:", bl[0])
else:
    print("FAIL: /bin/bash 提示符无 $/#:", rb.stdout[:300])
sl = [l for l in rs.stdout.splitlines() if "@" in l and ">" in l]
if sl:
    print("OK: /bin/sh 提示符保持 '>' 风格:", sl[0])
else:
    print("FAIL: /bin/sh 提示符无 '>':", rs.stdout[:300])
# 两种启动都必须正常退出(无 segfault)
if rb.returncode == 0 and rs.returncode == 0:
    print("OK: sh/bash stdin EOF 均正常退出(无崩溃)")
else:
    print("FAIL: 退出码 sh=%d bash=%d" % (rs.returncode, rb.returncode))
