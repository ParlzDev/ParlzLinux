#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import os
import opk, cfg, opkgcl

opk.regress_init()

arch_line1 = 'arch all 1'
arch_line2 = 'arch any 2'
expected = arch_line1 + '\n' + arch_line2 + '\n'

with open('test_arch1.conf', 'w') as f:
    f.write(arch_line1)
with open('test_arch2.conf', 'w') as f:
    f.write(arch_line2)

(status, output) = opkgcl.opkgcl('-f test_arch1.conf -f test_arch2.conf print_architecture')
if status != 0:
    opk.fail("with an error '{}'".format(output))
elif output != expected:
    opk.fail("output didn't match.\nexpected:\n{}\nactual:\n{}".format(expected, output))

os.remove('test_arch1.conf')
os.remove('test_arch2.conf')
