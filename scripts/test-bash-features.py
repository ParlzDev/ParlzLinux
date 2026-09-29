#!/usr/bin/env python3
"""逐项验证用户要求的 Bash 特性，不依赖待测 shell 自己判定结果。"""
import os
import pathlib
import subprocess
import sys
import tempfile

shell = str(pathlib.Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="parlz-bash-test-") as td:
    root = pathlib.Path(td)
    (root / "source.sh").write_text("sourced=SOURCE_OK\n")
    (root / "deep/sub").mkdir(parents=True)
    (root / "deep/a.txt").write_text("a")
    (root / "deep/sub/b.txt").write_text("b")
    cases = [
        ("数组", 'arr=(a "b c" d); printf "<%s>\\n" "${arr[@]}"', '<a>\n<b c>\n<d>\n'),
        ("[[ ]] 条件", 'v="hello world"; [[ $v == hello* && $v =~ world$ ]]; echo $?', '0\n'),
        ("ANSI-C 转义", "printf '%s' $'A\\nB\\tC'", 'A\nB\tC'),
        ("大括号扩展", 'echo {1..10}', '1 2 3 4 5 6 7 8 9 10\n'),
        ("function", 'function f { echo FUNC_OK; }; f', 'FUNC_OK\n'),
        ("local 作用域", 'v=outer; f() { local v=inner; echo "$v"; }; f; echo "$v"', 'inner\nouter\n'),
        ("进程替换", '/bin/cat <(echo PROC_OK)', 'PROC_OK\n'),
        ("globstar", 'shopt -s globstar; printf "%s\\n" deep/**/*.txt', 'deep/a.txt\ndeep/sub/b.txt\n'),
        ("source", 'source ./source.sh; echo "$sourced"', 'SOURCE_OK\n'),
        ("echo -e", r'echo -e "A\nB\tC"', 'A\nB\tC\n'),
        ("[ ] 中 ==", '[ x == x ]; echo $?; [ x == y ]; echo $?', '0\n1\n'),
    ]
    failed = 0
    for name, code, expected in cases:
        env = {**os.environ, "HOME": td, "LC_ALL": "C", "PATH": "/usr/bin:/bin"}
        try:
            p = subprocess.run([shell, '-c', code], cwd=td, env=env,
                               capture_output=True, text=True, timeout=5)
            good = p.returncode == 0 and p.stdout == expected
            print(('PASS' if good else 'FAIL') + ' ' + name)
            if not good:
                print(f'  rc={p.returncode} stdout={p.stdout!r} stderr={p.stderr[:500]!r}')
                failed += 1
        except subprocess.TimeoutExpired:
            print('FAIL ' + name + ' 超时')
            failed += 1
    print(f'结果: {len(cases)-failed}/{len(cases)} 通过')
    sys.exit(bool(failed))
