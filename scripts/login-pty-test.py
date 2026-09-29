#!/usr/bin/env python3
# login-pty-test.py - pty 驱动 userland login 静态二进制, 验收三条路径。
# 用法: python3 login-pty-test.py [login_bin]
# 宿主上跑会读写 /etc/parlz-auth(需可写), 先备份后恢复。
import os, pty, sys, time, stat, select, shutil, subprocess

BIN = sys.argv[1] if len(sys.argv) > 1 else "/home/jgzyes/parlz-userland/build/bin/login"
# user 命令与 login 同目录(多账户用例要用它建第二个账户)
USERBIN = os.path.join(os.path.dirname(BIN), "user")
# login 认证通过后写下的用户名(boot body 读它导出 $USER)
WHOAMI = "/tmp/.parlz-login-user"

class Pty:
    def __init__(self):
        self.m, self.s = pty.openpty()
        self.pid = os.fork()
        if self.pid == 0:
            os.setsid()
            os.dup2(self.s, 0); os.dup2(self.s, 1); os.dup2(self.s, 2)
            os.close(self.s)
            os.execv(BIN, ["login"])
            os._exit(127)
        os.close(self.s)
        self.buf = b""
        self.st = None
    def drain(self, t=0.5):
        got = b""; end = time.time() + t
        while time.time() < end:
            try:
                r, _, _ = select.select([self.m], [], [], 0.1)
            except OSError:
                break
            if r:
                try:
                    d = os.read(self.m, 65536)
                except OSError:
                    break
                if not d:
                    break
                got += d; end = time.time() + t
            elif got:
                break
        self.buf += got
        return got.decode(errors="ignore")
    def send(self, s):
        os.write(self.m, s.encode() + b"\n")
    def wait(self, pats, t=8):
        end = time.time() + t
        while time.time() < end:
            self.drain(0.4)
            tail = self.buf[-400:].decode(errors="ignore")
            for p in pats:
                if p in tail:
                    return True
            try:
                wp, st = os.waitpid(self.pid, os.WNOHANG)
                if wp:
                    self.st = st
                    return False
            except ChildProcessError:
                return False
        return False
    def finish(self):
        try:
            _, self.st = os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass
        try:
            os.close(self.m)
        except OSError:
            pass
    @property
    def rc(self):
        if self.st is None:
            return -1
        return os.WEXITSTATUS(self.st) if os.WIFEXITED(self.st) else -1

AUTH = "/etc/parlz-auth"
bak = None
if os.path.exists(AUTH):
    bak = AUTH + ".bak"
    shutil.move(AUTH, bak)

def acct_lines(text):
    """账户行(跳过注释与空行)。文件头有格式说明注释, 判据只认账户行。"""
    return [l for l in text.splitlines()
            if l.strip() and not l.strip().startswith("#")]

try:
    # S1 首次设置 tester/pw123
    s = Pty()
    assert s.wait(["首次进入", "Username:"]), "S1 无首次提示"
    s.send("tester"); s.wait(["Password:"])
    s.send("pw123"); s.wait(["Confirm"])
    s.send("pw123"); s.wait(["已创建", "不一致", "不能为空", "完成", "拒绝"])
    s.finish(); rc1 = s.rc
    authok = os.path.exists(AUTH)
    content = open(AUTH).read() if authok else ""
    # 存的必须是 $6$ 散列, 不能还有明文(整盘 IMG 是公开下载物);
    # 新格式是每行 "用户名:口令"
    acc = acct_lines(content)
    s1 = (rc1 == 0 and authok and len(acc) == 1
          and acc[0].startswith("tester:$6$") and "pw123" not in content)
    print("S1:", "PASS" if s1 else "FAIL",
          "rc=%d auth=%s head=%s" % (rc1, authok, content[:18].replace("\n", "/")))

    # S2 正确登录
    s = Pty()
    assert s.wait(["Parlz login: Username:"]), "S2 无登录提示"
    s.send("tester"); s.wait(["Password:"])
    s.send("pw123"); s.wait(["欢迎", "失败", "拒绝", "错误"])
    s.finish(); rc2 = s.rc
    s2 = rc2 == 0
    print("S2:", "PASS" if s2 else "FAIL", "rc=%d" % rc2)

    # S3 错密码 x3 拒绝
    s = Pty()
    assert s.wait(["Parlz login: Username:"]), "S3 无登录提示"
    for i in range(3):
        s.send("tester"); s.wait(["Password:"])
        s.send("wrongpass")
        if i < 2:
            s.wait(["请重试", "失败", "错误"])
        else:
            s.wait(["拒绝", "拒绝登录", "错误"])
    s.finish(); rc3 = s.rc
    s3 = rc3 == 1
    print("S3:", "PASS" if s3 else "FAIL", "rc=%d" % rc3)

    # S4/S5 长口令(>31 字符)。回归用例: 确认那一步曾复用用户名缓冲
    # (32 字节), read_secret 静默截断到 31 -> 两次永不相等 -> 长密码根本
    # 设不上。旧代码在这里必红。
    LONG = "correct-horse-battery-staple-0123456789ABCD"
    if os.path.exists(AUTH):
        os.remove(AUTH)
    s = Pty()
    assert s.wait(["首次进入", "Username:"]), "S4 无首次提示"
    s.send("jgz"); s.wait(["Password:"])
    s.send(LONG); s.wait(["Confirm"])
    s.send(LONG); s.wait(["已创建", "不一致", "不能为空", "完成", "拒绝"])
    s.finish(); rc4 = s.rc
    authok4 = os.path.exists(AUTH)
    c4 = open(AUTH).read() if authok4 else ""
    # 关键回归点: 长口令要**完整**参与散列(旧代码确认那步截到 31 位)
    acc4 = acct_lines(c4)
    s4 = (rc4 == 0 and authok4 and len(acc4) == 1
          and acc4[0].startswith("jgz:$6$") and LONG not in c4)
    print("S4:", "PASS" if s4 else "FAIL",
          "rc=%d auth=%s head=%s" % (rc4, authok4, c4[:14].replace("\n", "/")))

    # S5 用长口令登录(S4 没建成凭证文件时记 FAIL, 不 assert 崩掉整轮)
    s = Pty()
    if not s.wait(["Parlz login: Username:"]):
        s.finish()
        s5 = False
        print("S5: FAIL(没有登录提示 —— S4 没能创建 /etc/parlz-auth)")
    else:
        s.send("jgz"); s.wait(["Password:"])
        s.send(LONG); s.wait(["欢迎", "失败", "拒绝", "错误"])
        s.finish(); rc5 = s.rc
        s5 = rc5 == 0
        print("S5:", "PASS" if s5 else "FAIL", "rc=%d" % rc5)

    # S6 旧格式(明文, 两行)凭证: 已经分发出去的 IMG 里就是那样,
    #   必须还能登进去, 并且成功登录当场迁到新格式($6$ 散列 + "名:密" 一行)。
    with open(AUTH, "w") as f:
        f.write("olduser\noldpass\n")
    os.chmod(AUTH, 0o600)
    s = Pty()
    if not s.wait(["Parlz login: Username:"]):
        s.finish()
        s6 = False
        print("S6: FAIL(没有登录提示)")
    else:
        s.send("olduser"); s.wait(["Password:"])
        s.send("oldpass"); s.wait(["欢迎", "失败", "拒绝", "错误"])
        s.finish(); rc6 = s.rc
        c6 = open(AUTH).read()
        acc6 = acct_lines(c6)
        s6 = (rc6 == 0 and len(acc6) == 1
              and acc6[0].startswith("olduser:$6$") and "oldpass" not in c6)
        print("S6:", "PASS" if s6 else "FAIL",
              "rc=%d head=%s" % (rc6, c6[:20].replace("\n", "/")))

    # S7 升级后原口令照样登得进(证明存的是同一个口令的散列, 不是放行兜底)
    s = Pty()
    if not s.wait(["Parlz login: Username:"]):
        s.finish()
        s7 = False
        print("S7: FAIL(没有登录提示)")
    else:
        s.send("olduser"); s.wait(["Password:"])
        s.send("oldpass"); s.wait(["欢迎", "失败", "拒绝", "错误"])
        s.finish(); rc7 = s.rc
        s7 = rc7 == 0
        print("S7:", "PASS" if s7 else "FAIL", "rc=%d" % rc7)

    # S8/S9 多账户: 登录第二个账户必须认自己 —— 而不是"文件第一行那个"。
    # 回归点: boot body 早先是用 `head -1 /etc/parlz-auth` 当登录名的,
    # 单账户时看不出来, 多账户下必然张冠李戴。顺带验 login 把**认证过的**
    # 用户名写进 PA_WHOAMI_PATH(body 靠它导出 $USER)。
    if os.path.exists(AUTH):
        os.remove(AUTH)
    s = Pty()
    assert s.wait(["首次进入", "Username:"]), "S8 无首次提示"
    s.send("alice"); s.wait(["Password:"])
    s.send("alicepw"); s.wait(["Confirm"])
    s.send("alicepw"); s.wait(["已创建", "不一致", "不能为空"])
    s.finish()
    subprocess.run([USERBIN, "add", "bob", "bobpw"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def login_as(u, p):
        t = Pty()
        if not t.wait(["Parlz login: Username:"]):
            t.finish(); return None
        t.send(u); t.wait(["Password:"])
        t.send(p); t.wait(["欢迎", "失败", "拒绝", "错误"])
        t.finish()
        who = None
        if os.path.exists(WHOAMI):
            who = open(WHOAMI).read().strip()
            os.remove(WHOAMI)
        return (t.rc, who)

    r8 = login_as("bob", "bobpw")
    s8 = r8 == (0, "bob")
    print("S8:", "PASS" if s8 else "FAIL", "login=%s" % (r8,))

    r9 = login_as("alice", "alicepw")
    s9 = r9 == (0, "alice")
    print("S9:", "PASS" if s9 else "FAIL", "login=%s" % (r9,))

    ok = s1 and s2 and s3 and s4 and s5 and s6 and s7 and s8 and s9
    print("LOGIN_PTY_ALL_OK" if ok else "LOGIN_PTY_FAIL")
finally:
    if os.path.exists(AUTH):
        os.remove(AUTH)
    if bak and os.path.exists(bak):
        os.rename(bak, AUTH)
sys.exit(0 if ok else 1)
