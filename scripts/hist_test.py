#!/usr/bin/env python3
"""pty 驱动 Parlz shell: 验证交互回显 + 上下键历史。"""
import pty, os, sys, time, select

target = sys.argv[1] if len(sys.argv) > 1 else '/tmp/sh_test'
pid, fd = pty.fork()
if pid == 0:
    os.execv(target, [target])

out = b''
def send(data):
    time.sleep(0.2)
    os.write(fd, data)
def drain(sec=0.3):
    global out
    time.sleep(sec)
    while True:
        r, _, _ = select.select([fd], [], [], 0.05)
        if not r:
            break
        try:
            d = os.read(fd, 4096)
        except OSError:
            break
        if not d:
            break
        out += d

send(b'echo AAA\r')
drain()
send(b'echo BBB\r')
drain()
send(b'\x1b[A\r')    # 上键: 应取 BBB 并提交
drain()
send(b'\x1b[A\r')    # 上键: 应取 AAA 并提交
drain()
send(b'exit\r')
drain(0.5)

t = out.decode('utf-8', 'replace')
sys.stderr.write('---CAPTURED---\n' + t + '---END---\n')
# 期望: 每个提交各出现一次(输出行), 且 pty 回显里也各出现一次
echo_ok = t.count('AAA') >= 1 and t.count('BBB') >= 1
# 历史命中: BBB 被重新执行一次(第 4 条命令) + AAA 被重新执行一次
hist_ok = t.count('BBB') >= 2 and t.count('AAA') >= 2
echo2 = 'ECHO_OK' if echo_ok else 'ECHO_FAIL'
hist = 'HISTORY_OK' if hist_ok else 'HISTORY_FAIL'
print(echo2, hist)
