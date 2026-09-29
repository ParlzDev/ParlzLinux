#!/usr/bin/env python3
# 直接运行 gen-fatboot 的 Python 段, 调试字节数
import re
src = open("/mnt/f/Linux/Parlz/scripts/gen-fatboot.sh").read()
i = src.find("<<'PYEOF'")
j = src.find("\nPYEOF", i)
code = src[i+8:j]
open("/tmp/gf-debug.py", "w").write(code)
print("python extracted:", len(code))
