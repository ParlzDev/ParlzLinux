#!/usr/bin/env python3
# user-cmd-test.py - `user`(账户增删改)验收。
#
# 判据都取**终态**, 不看命令自己打的字:
#   - /etc/parlz-auth 里账户行真的变了(且是 $6$ 散列, 无明文);
#   - 用 login 真登一次: 该进的进(rc 0 + whoami 认的是本人), 该拒的拒(rc 1)。
# 用法: python3 user-cmd-test.py [user_bin]   (同目录要有 login)
import os, pty, sys, time, select, shutil, subprocess

# 输出实时刷(整条测试要跑几十秒; 全缓冲会让人以为挂死了)
try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass

BIN = sys.argv[1] if len(sys.argv) > 1 else "/home/jgzyes/parlz-userland/build/bin/user"
LOGIN = os.path.join(os.path.dirname(BIN), "login")
AUTH = "/etc/parlz-auth"
WHOAMI = "/tmp/.parlz-login-user"

def accts():
    if not os.path.exists(AUTH):
        return {}
    out = {}
    for ln in open(AUTH):
        ln = ln.strip()
        if not ln or ln.startswith("#") or ":" not in ln:
            continue
        k, v = ln.split(":", 1)
        out[k] = v
    return out

def run(*args, feed=None, timeout=20):
    """跑 user 命令。
    feed=None  → 直接参数式(非 tty), 判退出码;
    feed=[(等到的提示, 该发的行), …] → pty 交互式。
    ★ 必须"看到提示再发下一行": pa_read_pass 进 raw 模式用 TCSAFLUSH,
      会把**已经排在输入队列里**的字符丢掉 —— 一次把三行全塞进去,
      密码读就会永远等不到输入(死等到超时)。"""
    if feed is None:
        p = subprocess.run([BIN, *args], capture_output=True, timeout=timeout)
        return p.returncode, (p.stdout + p.stderr).decode("utf-8", "replace")
    m, s = pty.openpty()
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2)
        os.close(s)
        os.execv(BIN, [BIN, *args])
        os._exit(127)
    os.close(s)
    buf = b""
    def drain(ms):
        nonlocal buf
        end = time.time() + ms / 1000.0
        while time.time() < end:
            r, _, _ = select.select([m], [], [], 0.1)
            if r:
                try: buf += os.read(m, 4096)
                except OSError: return
                end = time.time() + 0.25
    for wait_for, text in feed:
        end = time.time() + timeout
        while time.time() < end:
            drain(150)
            if wait_for in buf.decode("utf-8", "replace"):
                break
        os.write(m, (text + "\n").encode())
        drain(250)
    drain(600)
    try:
        _, st = os.waitpid(pid, 0)
        rc = os.WEXITSTATUS(st) if os.WIFEXITED(st) else -1
    except ChildProcessError:
        rc = -1
    drain(150)
    os.close(m)
    return rc, buf.decode("utf-8", "replace")

def login_rc(u, p, tries=3, timeout=30):
    """用 login 真登一次: 返回 (rc, whoami)。rc: 0=欢迎, 1=拒绝, -1=没走到判定。

    ★ 必须按**提示**喂、最多喂 tries 轮: login 一次拒绝后还会再问 2 次,
      只喂一轮会让它停在 "Username:" 上, 而 waitpid 会一直等下去(踩过:
      整条测试挂死 6 分钟)。超时一律 SIGKILL 收尸, 绝不无限等。"""
    if os.path.exists(WHOAMI):
        os.remove(WHOAMI)
    m, s = pty.openpty()
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2)
        os.close(s)
        os.execv(LOGIN, [LOGIN])
        os._exit(127)
    os.close(s)
    buf = b""
    sent_u = sent_p = 0
    deadline = time.time() + timeout
    verdict = None
    while time.time() < deadline:
        r, _, _ = select.select([m], [], [], 0.2)
        if r:
            try:
                buf += os.read(m, 4096)
            except OSError:
                break
        t = buf.decode("utf-8", "replace")
        while sent_u < tries and t.count("Username:") > sent_u:
            os.write(m, (u + "\n").encode())
            sent_u += 1
        while sent_p < sent_u and t.count("Password:") > sent_p:
            os.write(m, (p + "\n").encode())
            sent_p += 1
        if "欢迎" in t:
            verdict = 0
            break
        if "拒绝登录" in t:
            verdict = 1
            break
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    for _ in range(50):
        try:
            if os.waitpid(pid, os.WNOHANG)[0]:
                break
        except ChildProcessError:
            break
        time.sleep(0.02)
    os.close(m)
    who = open(WHOAMI).read().strip() if os.path.exists(WHOAMI) else None
    if os.path.exists(WHOAMI):
        os.remove(WHOAMI)
    return (verdict if verdict is not None else -1), who

def login_first_run(timeout=8):
    """账户表空掉之后, login 应回到"首次进入"设置(不许把系统锁死)。"""
    m, s = pty.openpty()
    pid = os.fork()
    if pid == 0:
        os.setsid()
        os.dup2(s, 0); os.dup2(s, 1); os.dup2(s, 2)
        os.close(s)
        os.execv(LOGIN, [LOGIN])
        os._exit(127)
    os.close(s)
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([m], [], [], 0.2)
        if r:
            try:
                buf += os.read(m, 4096)
            except OSError:
                break
        if "首次进入" in buf.decode("utf-8", "replace"):
            break
    try:
        os.kill(pid, 9)
    except ProcessLookupError:
        pass
    for _ in range(50):
        try:
            if os.waitpid(pid, os.WNOHANG)[0]:
                break
        except ChildProcessError:
            break
        time.sleep(0.02)
    os.close(m)
    return "首次进入" in buf.decode("utf-8", "replace")


bak = None
if os.path.exists(AUTH):
    bak = AUTH + ".bak"
    shutil.move(AUTH, bak)
if os.path.exists(AUTH):
    os.remove(AUTH)
res = []
def check(name, cond, extra=""):
    res.append(cond)
    print(f"{name}: {'PASS' if cond else 'FAIL'} {extra}")

try:
    # U1 参数式 add
    rc, out = run("add", "carol", "carolpw")
    a = accts()
    check("U1 add 参数式", rc == 0 and "carol" in a
          and a.get("carol", "").startswith("$6$") and "carolpw" not in a.get("carol", ""),
          "rc=%d acct=%s" % (rc, list(a)))
    r, w = login_rc("carol", "carolpw")
    check("U1b 新账户能登", r == 0 and w == "carol", "login=%s/%s" % (r, w))

    # U2 重复 add 必须拒
    n0 = len(a)
    rc, out = run("add", "carol", "other")
    check("U2 重复 add 拒绝", rc != 0 and len(accts()) == n0, "rc=%d" % rc)

    # U3 非法用户名
    rc1, _ = run("add", "a:b", "x")
    rc2, _ = run("add", "a b", "x")
    check("U3 非法名拒绝", rc1 != 0 and rc2 != 0, "rc=%d/%d" % (rc1, rc2))

    # U4 参数式 upd: 老口令必须失效, 新口令必须能进
    rc, out = run("upd", "carol", "newpw123")
    r_old, _ = login_rc("carol", "carolpw")
    r_new, w_new = login_rc("carol", "newpw123")
    check("U4 upd 参数式(旧口令失效/新口令可用)",
          rc == 0 and r_old == 1 and r_new == 0 and w_new == "carol",
          "rc=%d old=%d new=%d" % (rc, r_old, r_new))

    # U5 rm: 账户真的没了; 表清空后 login 回到"首次进入"(不是把系统锁死)
    rc, out = run("rm", "carol")
    gone = "carol" not in accts()
    fr = login_first_run()
    check("U5 rm 后账户消失且回到首次设置",
          rc == 0 and gone and fr,
          "rc=%d gone=%s first_run=%s" % (rc, gone, fr))

    # U6 删不存在的账户
    rc, out = run("rm", "nosuchuser")
    check("U6 rm 不存在的账户拒绝", rc != 0, "rc=%d" % rc)

    # U7 交互式 add(不带给参数)
    rc, out = run("add", feed=[("Username:", "dave"), ("Password:", "davepw"),
                               ("Confirm", "davepw")])
    r, w = login_rc("dave", "davepw")
    check("U7 add 交互式", rc == 0 and r == 0 and w == "dave",
          "rc=%d login=%s/%s" % (rc, r, w))

    # U8 交互式 upd
    rc, out = run("upd", feed=[("Username:", "dave"), ("口令", "davepw2"),
                               ("Confirm", "davepw2")])
    r_old, _ = login_rc("dave", "davepw")
    r_new, w = login_rc("dave", "davepw2")
    check("U8 upd 交互式", rc == 0 and r_old == 1 and r_new == 0 and w == "dave",
          "rc=%d old=%d new=%d" % (rc, r_old, r_new))

    # U9 交互式 add 两次口令不一致必须拒
    rc, out = run("add", feed=[("Username:", "erin"), ("Password:", "onepw"),
                               ("Confirm", "twopw")])
    check("U9 交互式两次不一致拒绝", rc != 0 and "erin" not in accts(), "rc=%d" % rc)

    # U10 裸 user: 列出账户
    rc, out = run()
    check("U10 裸 user 列账户", "dave" in out, "rc=%d" % rc)
finally:
    if os.path.exists(AUTH):
        os.remove(AUTH)
    if bak and os.path.exists(bak):
        os.rename(bak, AUTH)

print("USER_CMD_ALL_OK" if all(res) else "USER_CMD_FAIL")
sys.exit(0 if all(res) else 1)
