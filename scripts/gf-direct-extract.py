#!/usr/bin/env python3
# 直接跑 gen-fatboot.sh 的 Python 段(提取 heredoc), 调试镜像字节数。
import sys, re
src = open("/mnt/f/Linux/Parlz/scripts/gen-fatboot.sh").read()
m = re.search(r"python3 - .*?<<'PYEOF'\n(.*)\nPYEOF", src, re.S)
if not m:
    sys.exit("heredoc 段提取失败")
code = m.group(1)
# 把 heredoc 里的 argv 解析换成直接参数
code = code.replace(
    'sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],\n'
    '    sys.argv[5], sys.argv[6], int(sys.argv[7]), int(sys.argv[8]), sys.argv[9]',
    'sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],\n'
    '    sys.argv[5], sys.argv[6], int(sys.argv[7]), int(sys.argv[8]), sys.argv[9]')
open("/tmp/gf-direct.py", "w").write(code)
sys.exit(0)
