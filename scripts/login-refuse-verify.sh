#!/bin/sh
# login-refuse-verify.sh - 验"认证失败不许进 shell"(真实 QEMU + 真 body)。
#
# 修的缺陷(用户报的): body 以前**不看 login 的退出码** —— 3 次输错之后照样
# 往下走 into shell, 而且 USER 是用 `head -1 /etc/parlz-auth` 取的
# (多账户下张冠李戴: 登进来的人是谁根本没问过)。
#
# 判据(都在一次启动里):
#   ① 连喂 3 次错口令 → 串口出现"拒绝登录";
#   ② 此刻日志里**不许**出现 "Parlz shell"(旧代码在这必红);
#   ③ 再喂正确凭证(body 的 2 秒重试) → 出现 "欢迎" + "Parlz shell";
#   ④ shell 里 `echo WHO=$USER` 回显的是**认证过的**账户名。
#
# 做法: 把一份预置好的 /etc/parlz-auth(带 $6$ 散列)塞进 initramfs 副本,
# 起 QEMU 从串口喂凭证。不动 images/ 下的正式产物。
# 用法: wsl -d Ubuntu-24.04 -u root -e bash -c "sh /mnt/f/Linux/Parlz/scripts/login-refuse-verify.sh"
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
P=/mnt/f/Linux/Parlz
IMG=$P/images
W=/home/jgzyes/login-refuse
rm -rf "$W"; mkdir -p "$W"
for f in "$IMG/parlz-bzImage" "$IMG/parlz-initramfs"; do
    [ -f "$f" ] || { echo "缺 $f(先 build-kernel.sh)"; exit 1; }
done

USER_A=alice
PASS_A=alice-good-pw
USER_B=bob
PASS_B=bob-good-pw

# 两个账户, 都是 $6$ 散列; **把 bob 放第一行** —— 旧逻辑取的就是第一行,
# 这样"登进 alice 却报 bob"才会暴露。
# hash_of <口令> <盐> —— 别把用户名当口令传进去(踩过: 存的是用户名的散列,
# 于是一路"密码错", 而产品侧完全无辜)
hash_of() {
    openssl passwd -6 -salt "$2" "$1" 2>/dev/null
}
H1=$(hash_of "$PASS_B" bobsalt0123456789)
H2=$(hash_of "$PASS_A" alicesalt012345678)
[ -n "$H1" ] && [ -n "$H2" ] || { echo "openssl passwd -6 不可用"; exit 1; }

echo ">>> [1] 造带账户表的 initramfs 副本"
cd "$W" && rm -rf root && mkdir root && cd root
zcat "$IMG/parlz-initramfs" | cpio -idm 2>/dev/null
mkdir -p etc
printf '# test accounts\n%s:%s\n%s:%s\n' "$USER_B" "$H1" "$USER_A" "$H2" > etc/parlz-auth
chmod 600 etc/parlz-auth
find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$W/initramfs-test.cpio.gz"
[ -s "$W/initramfs-test.cpio.gz" ] || { echo "重打 initramfs 失败"; exit 1; }
echo "    账户: $USER_B(第一行) / $USER_A(第二行), 口令各不同"

echo ">>> [2] 起 QEMU(串口喂凭证)"
LOG=$W/serial.log
FIFO=$W/in.fifo
rm -f "$FIFO"; mkfifo "$FIFO"
exec 3<>"$FIFO"          # 读写打开: 纯写会与 qemu 的读端互等
qemu-system-x86_64 -m 1024M -nographic -no-reboot \
    -kernel "$IMG/parlz-bzImage" -initrd "$W/initramfs-test.cpio.gz" \
    -append "console=ttyS0,115200 install.skip=1" \
    <"$FIFO" >"$LOG" 2>&1 &
QPID=$!

wait_for() {   # wait_for <串> <超时秒> [至少出现几次]
    _want=${3:-1}; _i=0
    while [ $_i -lt $(( $2 * 5 )) ]; do
        _n=$(grep -ac "$1" "$LOG" 2>/dev/null || true)
        [ "${_n:-0}" -ge "$_want" ] && return 0
        grep -qa "Kernel panic" "$LOG" 2>/dev/null && return 2
        _i=$((_i + 1)); sleep 0.2
    done
    return 1
}

FAIL=""
wait_for "Username:" 90 || { echo "没等到登录提示"; tail -20 "$LOG"; exec 3>&-; kill $QPID 2>/dev/null; exit 1; }
echo "    登录提示已出现(第 ${_i} 次轮询)"

# ① 三次错口令(login 自己给 3 次机会)。
# ★ 必须**盯着提示喂**: login 一轮内有 3 次机会、拒绝后 body 再重开一轮,
#   用固定 sleep 会喂空(踩过: 正确凭证被当成第 2/3 次错误尝试吃掉)。
login_round() {   # login_round <用户> <口令> <第几轮>
    # 每轮的提示是成对的(U1,P1,U2,P2,…), 按**出现次数**等, 不按固定 sleep
    wait_for "Username:" 30 "$3"  || return 1
    printf '%s\n' "$1" >&3
    wait_for "Password:" 15 "$3"  || return 1
    printf '%s\n' "$2" >&3
    sleep 1
    return 0
}
for i in 1 2 3; do
    login_round "$USER_A" "wrong-password-$i" "$i" || FAIL="$FAIL 第 $i 次凭证喂不进去"
done
wait_for "拒绝登录" 30 || FAIL="$FAIL 三次错口令后没有出现拒绝登录"

# ② 此刻绝不允许进 shell
sleep 3
if grep -qa "Parlz shell" "$LOG"; then
    FAIL="$FAIL 认证失败却进了 shell(旧缺陷回归)"
fi
grep -qa "认证失败,2 秒后重试" "$LOG" || FAIL="$FAIL 没有重试提示(body 没拦住?)"

# ③ 正确凭证: body 会再开一轮, 同样盯着提示喂
login_round "$USER_A" "$PASS_A" 4 || FAIL="$FAIL 正确凭证没喂进去"
wait_for "Parlz shell" 40 || FAIL="$FAIL 正确凭证也进不了 shell"
grep -qa "欢迎 $USER_A" "$LOG" || FAIL="$FAIL 没有欢迎 $USER_A"

# ④ USER 必须是认证过的那个人(不是文件第一行的 bob)
wait_for "root@parlz" 20 || true
printf 'echo WHO=$USER\n' >&3
wait_for "WHO=" 20 || FAIL="$FAIL 没等到 WHO= 回显"
grep -qa "WHO=$USER_A" "$LOG" || FAIL="$FAIL \$USER 不是认证账户(alice)"
if grep -qa "WHO=$USER_B" "$LOG"; then
    FAIL="$FAIL \$USER 成了文件第一行的账户(bob)"
fi

exec 3>&-
kill $QPID 2>/dev/null || true; wait $QPID 2>/dev/null || true

echo "--- 关键行 ---"
grep -a "Parlz login\|拒绝登录\|认证失败\|欢迎\|WHO=\|Parlz shell" "$LOG" | tail -14 | sed 's/^/    /'

if [ -n "$FAIL" ]; then
    echo "login-refuse-verify: FAIL ->$FAIL"
    tail -25 "$LOG"
    exit 1
fi
echo "login-refuse-verify: PASS(3 次错口令被拦在 shell 外; 正确凭证放行且 \$USER=认证账户)"
