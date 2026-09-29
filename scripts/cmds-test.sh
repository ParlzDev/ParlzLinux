#!/bin/sh
# cmds-test.sh - 核心命令语义回归(宿主侧直接跑静态二进制, 秒级, 不起 QEMU)。
#
# 起因: 命令地毯(scripts/selfcheck-guest.sh)发现 `cat < f` 什么都没输出 ——
# 查下来 `cat` 无参数时**根本不读 stdin**(POSIX 要求读), 于是
# `cat < f` / `... | cat` / `cat | grep x` 全是空输出。同族的 stdin 语义、
# 退出码、递归删除等一并钉住。
#
# 用法: sh scripts/cmds-test.sh [bin 目录]
#   默认 /home/jgzyes/parlz-userland/build/bin
set -u
BIN=${1:-/home/jgzyes/parlz-userland/build/bin}
[ -d "$BIN" ] || { echo "缺 bin 目录 $BIN"; exit 1; }
for c in cat grep awk rm mkdir chmod ln cp mv df ps; do
    [ -x "$BIN/$c" ] || { echo "缺 $BIN/$c"; exit 1; }
done
# ls / touch / clear 不是独立二进制: 它们是 parlz-sh 的内建(applet 软链指向它)
SH=$BIN/parlz-sh
[ -x "$SH" ] || SH=$BIN/sh
[ -x "$SH" ] || { echo "缺 parlz-sh"; exit 1; }
W=$(mktemp -d /tmp/cmds-test.XXXXXX); trap 'rm -rf "$W"' EXIT
cd "$W"
PASS=0; FAIL=0
ok()   { PASS=$((PASS + 1)); printf 'ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL %s\n     期望: [%s]\n     实得: [%s]\n' "$1" "$2" "$3"; }

# out <名字> <期望输出> <命令...>
out() {
    _n=$1; _w=$2; shift 2
    _g=$("$@" 2>/dev/null)
    [ "$_g" = "$_w" ] && ok "$_n" || bad "$_n" "$_w" "$_g"
}
# out_stdin <名字> <期望> <输入> <命令...>
out_stdin() {
    _n=$1; _w=$2; _in=$3; shift 3
    _g=$(printf '%s' "$_in" | "$@" 2>/dev/null)
    [ "$_g" = "$_w" ] && ok "$_n" || bad "$_n" "$_w" "$_g"
}
# rc <名字> <期望退出码> <命令...>
rc() {
    _n=$1; _w=$2; shift 2
    "$@" >/dev/null 2>&1; _g=$?
    [ "$_g" = "$_w" ] && ok "$_n" || bad "$_n" "rc=$_w" "rc=$_g"
}

printf 'abc\ndef\n' > f1.txt
printf 'x y\n' > f2.txt

# ---- cat: stdin 是 POSIX 语义, 老实现直接不读 ----
out_stdin 'cat|stdin'      'hello'        'hello'          "$BIN/cat"
out       'cat|file'       'abc
def'                                        "$BIN/cat" f1.txt
out       'cat|- 是 stdin'  'pipe'        sh -c "printf 'pipe' | '$BIN/cat' -"
out       'cat|两个文件'    'abc
def
x y'                                        "$BIN/cat" f1.txt f2.txt
rc        'cat 不存在的文件 rc=1' 1         "$BIN/cat" nosuch-file-zz

# ---- grep: 无文件读 stdin ----
out_stdin 'grep|stdin'     'x y'          'x y'            "$BIN/grep" y
out       'grep|文件'      'abc'                            "$BIN/grep" abc f1.txt
out       'grep|-v'        'def'                            "$BIN/grep" -v abc f1.txt
out       'grep|-c 只打数字' '1'                              "$BIN/grep" -c a f1.txt
rc        'grep 无匹配 rc=1' 1                              "$BIN/grep" zzz f1.txt

# ---- awk: 子集实现, 但**无文件时必须读 stdin**(以前 argc<3 直接 usage) ----
out_stdin 'awk|stdin 取列'  '42'           'a 42 b'         "$BIN/awk" '{print $2}'
out       'awk|文件取列'    'abc
def'                                          "$BIN/awk" '{print $1}' f1.txt
out_stdin 'awk|模式过滤'    '42'           'no
a 42 b'                                        "$BIN/awk" '/42/{print $2}'

# ---- ls / df / ps 冒烟 ----
_=$(ls_out=$("$SH" -c 'ls .' 2>/dev/null); case "$ls_out" in *f1.txt*) echo y;; esac)
[ "$_" = y ] && ok 'ls|列出文件(内建)' || bad 'ls|列出文件' '含 f1.txt' "$($SH -c 'ls .' 2>/dev/null)"
rc 'ls 不存在的路径 rc!=0' 1 "$SH" -c 'ls nosuch-dir-zz'
rc 'df rc=0' 0 "$BIN/df"
_=$(ps_out=$("$BIN/ps" 2>/dev/null); case "$ps_out" in *PID*|*bash*) echo y;; esac)
[ "$_" = y ] && ok 'ps|有输出' || bad 'ps|有输出' '含表头/进程' "$ps_out"

# ---- rm / mkdir / chmod / ln / cp / mv ----
rc 'rm -f 缺文件 rc=0' 0 "$BIN/rm" -f nosuch-file-zz
mkdir -p t1/t2; printf 'deep\n' > t1/t2/f3.txt
rc 'rm -r 递归删目录' 0 "$BIN/rm" -r t1
rc 'rm -r 后目录消失' 1 test -e t1
rc 'rm 普通删目录 rc!=0' 1 "$BIN/rm" .            # 拒绝删目录(要 -r)
rc 'mkdir -p 建多级' 0 "$BIN/mkdir" -p a/b/c
rc 'mkdir -p 幂等' 0 "$BIN/mkdir" -p a/b/c
"$BIN/chmod" 700 f2.txt
_g=$(stat -c %a f2.txt 2>/dev/null)
[ "$_g" = 700 ] && ok 'chmod 700' || bad 'chmod 700' 700 "$_g"
"$BIN/ln" -s f1.txt link1
out 'ln -s + cat 穿透' 'abc
def'                                        "$BIN/cat" link1
"$BIN/cp" f1.txt f3.txt
out 'cp 内容一致' 'abc
def'                                        "$BIN/cat" f3.txt
"$BIN/mv" f3.txt f4.txt
rc 'mv 后原名消失' 1 test -e f3.txt
out 'mv 后新名可读' 'abc
def'                                        "$BIN/cat" f4.txt

echo "----------------------------------------"
echo "cmds-test: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
