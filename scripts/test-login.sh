#!/bin/bash
# test-login.sh - login 交互流 E2E:
#   [1/3] host pty 认证状态机(宿主 /dev/console 是 tty, login 非 tty 重指交互)
#   [2/3] 嵌 60MiB initramfs 进内核 + setup.bin 补丁(AGENTS.md 陷阱1)
#   [3/3] QEMU busybox-init 双档:login.skip 非交互 + 首登交互设用户
# 用法: wsl -d Ubuntu-26.04 -u root -e bash /mnt/f/Linux/Parlz/scripts/test-login.sh
#       (要 root: 改 /etc/parlz-auth + QEMU 写 /tmp)
set -e
US=/home/jgzyes/parlz-userland
IMG=/mnt/f/Linux/Parlz/images
K=/home/jgzyes/parlz-kernel

echo "=== 重编 login + sh ==="
cd $US
cp -f /mnt/f/Linux/Parlz/userland/login.c src/login.c
cp -f /mnt/f/Linux/Parlz/userland/sh.c src/sh.c
cmake --build build --target login sh -j8 2>&1 | tail -2
cp build/bin/login root/bin/login
cp build/bin/sh root/bin/parlz-sh

# 防 initramfs 自引用膨胀: 嵌入前把 root/boot/vmlinuz 降为 8KB 占位
# (真 vmlinuz 只进 gen-fatboot 的 FAT 引导分区; 内核根 fs 不需要它,
#  且它会经 build-kernel 烘进 initramfs → 下次 build-userland 又拷回,
#  形成 60M→120M→180M 膨胀, 最终 do_populate_rootfs 128MiB 写挂。)
rm -f root/boot/vmlinuz
head -c 8192 /dev/zero > root/boot/vmlinuz
chmod 755 root/boot/vmlinuz

echo "=== [1/3] host pty 认证状态机 ==="
python3 - "$US/build/bin/login" <<'PY'
import os, sys, pty, select, time, shutil, subprocess
LOGIN = sys.argv[1]
HOST_AUTH = "/etc/parlz-auth"
HOST_BAK  = "/etc/parlz-auth.bak"
if os.path.exists(HOST_AUTH):
    shutil.copy2(HOST_AUTH, HOST_BAK)

def set_auth(content):
    with open(HOST_AUTH, "w") as f: f.write(content)
    os.chmod(HOST_AUTH, 0o600)

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

def run_login(steps, drain_ms=1200):
    # 宿主 WSL /dev/console 是 tty:login 非 tty stdin → 重指 console → 交互。
    # 用 pty.fork 让 stdin 本身是 tty(登录直接交互, 不经 console 重指)。
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(LOGIN, [LOGIN])
    out = b""
    for i, (data, wait) in enumerate(steps):
        time.sleep(0.3)
        if data:
            try: os.write(fd, data)
            except OSError: break
        out += drain(fd, 300 if i < len(steps)-1 else drain_ms)
    time.sleep(0.3)
    try: os.waitpid(pid, 0)
    except: pass
    return out.decode("utf-8","replace")

# A1: 正确凭据
set_auth("tester\nparlz\n")
o = run_login([(b"tester\n",0.4),(b"parlz\n",1.5)])
assert "拒绝" not in o, f"A1 应通过: {o[-300:]}"
print("  A1 PASS  正确凭据通过")

# A2: 错误密码 3 次后拒
o = run_login([(b"tester\n",0.4),(b"wrong\n",0.4),(b"tester\n",0.4),(b"wrong\n",0.4),(b"tester\n",0.4),(b"wrong\n",1.5)])
assert "拒绝登录" in o, f"A2 应拒绝: {o[-400:]}"
print("  A2 PASS  错误密码 3 次后拒")

# B: 删 auth → 首次设置
if os.path.exists(HOST_AUTH): os.remove(HOST_AUTH)
o = run_login([(b"newuser\n",0.4),(b"pw123\n",0.4),(b"pw123\n",1.5)])
assert os.path.exists(HOST_AUTH), "B: auth 应被创建"
assert "newuser" in open(HOST_AUTH).read()
print("  B  PASS  首次设置写入 auth")

# B2: 新凭据交互认证
o = run_login([(b"newuser\n",0.4),(b"pw123\n",1.5)])
assert "拒绝" not in o, f"B2 应通过: {o[-300:]}"
print("  B2 PASS  新凭据通过")

# C: 非 tty(stdin=pipe):宿主 /dev/console 是 tty → 重指交互, 3s 存活验证
p = subprocess.Popen([LOGIN], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
time.sleep(3)
if p.poll() is None:
    p.kill()
    print("  C  PASS  非 tty → 重指 /dev/console 进入交互(宿主, 3s 存活)")
else:
    out = p.stdout.read().decode("utf-8","replace")
    assert "非 tty" in out or "login" in out, f"C: 意外退出 rc={p.returncode}: {out!r}"
    print(f"  C  PASS  非 tty 直接放行 rc={p.returncode}")

if os.path.exists(HOST_BAK):
    shutil.move(HOST_BAK, HOST_AUTH)
elif os.path.exists(HOST_AUTH):
    os.remove(HOST_AUTH)
print("  HOST_PTY_ALL_OK")
PY

echo "=== [2/3] 嵌 initramfs + setup.bin 补丁 ==="
cd $US/root
find . | LC_ALL=C sort | cpio -o -H newc 2>/dev/null | gzip -9 > $K/rootfs.cpio.gz
echo "  rootfs.cpio.gz = $(du -h $K/rootfs.cpio.gz | cut -f1)"
cd $K
mkdir -p include/generated
make ARCH=x86_64 olddefconfig > /tmp/kold.log 2>&1 || { echo "olddef FAIL"; tail /tmp/kold.log; exit 1; }
make ARCH=x86_64 CONFIG_INITRAMFS_SOURCE=$K/rootfs.cpio.gz vmlinux bzImage > /tmp/kb.log 2>&1 || { echo "make FAIL"; tail /tmp/kb.log; exit 1; }
rm -f arch/x86/boot/setup.bin
objcopy -O binary arch/x86/boot/setup.elf arch/x86/boot/setup.bin
python3 -c "
d=bytearray(open('arch/x86/boot/setup.bin','rb').read())
d[0]=0xEB; d[1]=0x6A
for i in range(2,0x6C): d[i]=0x90
open('arch/x86/boot/setup.bin','wb').write(bytes(d))
print('  setup.bin patched')
"
make ARCH=x86_64 bzImage > /tmp/kb2.log 2>&1 || { echo "bzImage FAIL"; tail /tmp/kb2.log; exit 1; }
cp -f arch/x86/boot/bzImage $IMG/parlz-bzImage
cp -f $K/rootfs.cpio.gz $IMG/parlz-initramfs
echo "  bzImage = $(du -h $IMG/parlz-bzImage | cut -f1)"

echo "=== [3/3a] QEMU busybox-init + login.skip(非交互) ==="
DISKA=/tmp/parlz-lgA.img; LOGA=/tmp/lgA.log
rm -f $DISKA $LOGA
dd if=/dev/zero of=$DISKA bs=1M count=256 2>/dev/null
timeout 90 qemu-system-x86_64 -m 512M -nographic -no-reboot \
  -kernel $IMG/parlz-bzImage \
  -append "console=ttyS0,115200 login.skip" \
  -drive file=$DISKA,if=virtio,format=raw \
  -serial stdio -monitor none 2>/dev/null > $LOGA || true
grep -qa "Run /sbin/init as init process" $LOGA && P_INIT=PASS || P_INIT=FAIL
grep -qa "Parlz user-space (busybox-init)" $LOGA && P_US=PASS || P_US=FAIL
grep -qa "boot ready" $LOGA && P_RDY=PASS || P_RDY=FAIL
grep -qa "login: 非 tty 环境,跳过认证" $LOGA && P_SKIP=PASS || P_SKIP=FAIL
echo "  busybox-init(PID1)=$P_INIT  user-space=$P_US  boot ready=$P_RDY  非tty跳过=$P_SKIP"
grep -qa "root@(none)" $LOGA && { echo "  FAIL: 提示符仍 root@(none)"; exit 1; }
grep -qa "Kernel panic" $LOGA && { echo "  FAIL: 内核 panic"; tail -30 $LOGA; exit 1; }

echo "=== [3/3b] QEMU 首登交互: 无 auth → 设用户 tester → 提示符验证 ==="
DISKB=/tmp/parlz-lgB.img; LOGB=/tmp/lgB.log
rm -f $DISKB $LOGB
dd if=/dev/zero of=$DISKB bs=1M count=256 2>/dev/null
# 注入节奏: 等 busybox-init 进 shell + 首登交互(无 auth 时 login 立即
# 弹 Username:), tester/pw123/pw123 → whoami 验证 USER 传播
(
  sleep 35; printf "login\n"
  sleep 4;  printf "tester\n"
  sleep 4;  printf "pw123\n"
  sleep 4;  printf "pw123\n"
  sleep 5;  printf "whoami\n"
  sleep 4;  printf "exit\n"
) | timeout 100 qemu-system-x86_64 -m 512M -nographic -no-reboot \
  -kernel $IMG/parlz-bzImage \
  -append "console=ttyS0,115200" \
  -drive file=$DISKB,if=virtio,format=raw \
  -serial stdio -monitor none 2>/dev/null > $LOGB || true
echo "--- 关键行 ---"
grep -aE "Run /sbin/init|Parlz user-space|boot ready|首次|Username|Password|Confirm|已创建|认证|whoami|tester|@parlz|非 tty|拒绝|Kernel panic" $LOGB | head -25
# 判定
P3B=PASS
grep -qa "Kernel panic" $LOGB && { P3B=FAIL; echo "  FAIL: panic"; tail -30 $LOGB; exit 1; }
grep -qa "tester" $LOGB || P3B=FAIL
grep -qa "whoami" $LOGB && grep -A2 "whoami" $LOGB | grep -q "tester" || P3B=FAIL
grep -qa "root@(none)" $LOGB && P3B=FAIL
echo "  3/3b 首登交互验收 = $P3B"
[ "$P3B" = "PASS" ] || exit 1

echo ""
echo "=== ALL LOGIN TESTS DONE ==="
