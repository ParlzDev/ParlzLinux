#!/bin/sh
# sh-semantics-test.sh - parlz-sh 词法/语义回归(宿主侧跑, 秒级, 不起 QEMU)。
#
# 每一条都是**实测过的缺陷**或必须守住的语义, 不是随便挑的例子:
#   - 引号不保护空格/管道: `echo "a  b"` 曾输出 a b; `echo "a|b"` 曾被当管道劈开
#   - 赋值悬空: `X=hello` 之后 `echo $X` 曾是空的(putenv 存了指向局部缓冲的指针)
#   - 赋值只认大写: `x=1` 曾根本不是赋值
#   - `;` 不分句(交互路径): `echo a; echo b` 曾把 "a; echo b" 当 echo 的参数
#   - sh -c 盲切引号内的分号: `awk 'BEGIN{print 1; print 2}'` 曾被劈成两条
# 用法: sh scripts/sh-semantics-test.sh [sh 二进制]
#   默认 /home/jgzyes/parlz-userland/build/bin/sh
#   (构建目录里它叫 sh; 进 rootfs 才改名成 /bin/parlz-sh, 两个名字都认)
set -u
SH=${1:-/home/jgzyes/parlz-userland/build/bin/sh}
[ -x "$SH" ] || SH=/home/jgzyes/parlz-userland/build/bin/parlz-sh
[ -x "$SH" ] || { echo "缺 sh 二进制(试过 build/bin/sh 与 build/bin/parlz-sh)"; exit 1; }
W=$(mktemp -d /tmp/sh-sem.XXXXXX); trap 'rm -rf "$W"' EXIT
cd "$W"
PASS=0; FAIL=0

# t <名字> <期望> <代码>     — 用 sh -c 跑代码, 输出必须**完全等于**期望
#   期望里的 \n / \t 按转义解释(否则没法在多行期望里写换行)
t() {
    _name=$1; _want=$(printf '%b' "$2"); _code=$3
    _got=$("$SH" -c "$_code" 2>&1)
    if [ "$_got" = "$_want" ]; then
        PASS=$((PASS + 1)); printf 'ok   %s\n' "$_name"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL %s\n     期望: [%s]\n     实得: [%s]\n' "$_name" "$_want" "$_got"
    fi
}

# t_last <名字> <期望> <代码>  — 只比**最后一行**(给"命令会往 stderr 报错"的用例)
t_last() {
    _name=$1; _want=$2; _code=$3
    _got=$("$SH" -c "$_code" 2>/dev/null | /usr/bin/tail -1)
    if [ "$_got" = "$_want" ]; then
        PASS=$((PASS + 1)); printf 'ok   %s\n' "$_name"
    else
        FAIL=$((FAIL + 1))
        printf 'FAIL %s\n     期望: [%s]\n     实得: [%s]\n' "$_name" "$_want" "$_got"
    fi
}

# ---- 基本分词与引号 ----
t 'basic-args'      'hello world'        'echo hello world'
t 'quote-double'    'a  b'               'echo "a  b"'
t 'quote-single'    'c  d'               "echo 'c  d'"
t 'quote-keeps-pipe' 'a|b'               'echo "a|b"'
t 'quote-keeps-semi' 'a; b'              "echo 'a; b'"
t 'quote-empty'     'x y'                'echo x "" y'
t 'quote-joined'    'ab'                 'echo "a""b"'

# ---- 变量 ----
t 'assign-same-line' 'got=5'             'X=5; echo got=$X'
t 'assign-lowercase' 'got=5'             'x=5; echo got=$x'
t 'assign-quoted-val' 'got=a b'          'X="a b"; echo got=$X'
t 'assign-prefix-cmd' '1'                'A=2 env | /usr/bin/grep -c ^A=2'
t 'export-persist'   'z=9'               'export Z=9; echo z=$Z'
t 'brace-expansion'  'helloworld'        'X=hello; echo ${X}world'
t 'single-quote-no-expand' 'lit=$HOME'   "echo 'lit=\$HOME'"

# ---- 分句(; 与 ; 在引号内) ----
t 'semicolon-split'  'a\nb'              'echo a; echo b'
t 'semi-inside-awk'  '1\n2'               "awk 'BEGIN{print 1; print 2}'"
t 'semi-kept-in-quote' 'one; two'        "echo 'one; two'"

# ---- 重定向与管道 ----
t 'redir-out-in'     'abc'               'echo abc > f1; cat f1'
t 'redir-append'     'a\nb'              'echo a > f2; echo b >> f2; cat f2'
t 'redir-in'         'xyz'               'echo xyz > f3; cat < f3'
t 'pipe-basic'       'has x'              'echo "has x" | /usr/bin/grep has'
t 'pipe-two-stage'   'B'                  'printf "a\nB\n" | /usr/bin/grep B'

# ---- $? ----
t 'rc-after-ok'      'rc=0'               'echo ok > /dev/null; echo rc=$?'
t_last 'rc-after-fail' 'rc=1'             'cat /nonexistent-zz > /dev/null; echo rc=$?'

# ---- 跨行持久(X=hello 之后下一行还能读到) ----
printf 'X=hello\necho got=$X\ny=world\necho got2=$y\n' > cross.sh
_exp='got=hello
got2=world'
_got=$("$SH" cross.sh 2>/dev/null)
if [ "$_got" = "$_exp" ]; then
    PASS=$((PASS + 1)); echo "ok   cross-line-assign"
else
    FAIL=$((FAIL + 1))
    printf 'FAIL cross-line-assign\n     期望: [%s]\n     实得: [%s]\n' "$_exp" "$_got"
fi

echo "----------------------------------------"
echo "sh-semantics-test: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
