src = open('/mnt/f/Linux/Parlz/userland/opkg.c').read()
a = src.index('struct bitbuf')
b = src.index(' * 第二部分')
block = src[a:b]
end = block.rfind('\n}')
clean = block[:end + 2]
header = """#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#define MAXSYM 288
"""
main = open('/mnt/f/Linux/Parlz/scripts/def_main.c').read()
open('/tmp/def_test.c','w').write(header + clean + main)
print('built /tmp/def_test.c')
