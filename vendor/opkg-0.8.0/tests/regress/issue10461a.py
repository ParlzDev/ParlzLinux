#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Reporter: Gabe Jones
#
# What steps will reproduce the problem?
# ======================================
#
# 1.- Create package 'a' and 'b', which install files to
#     a shared new directory.
# 2.- Install package 'b', followed by package 'a'.
# 3.- Remove package 'b', followed by package 'a'.
#
# What is the expected output? What do you see instead?
# ======================================
#
# The shared new directory is deleted when the package 'a'
# is uninstalled.
# The shared directory is orphaned and not deleted after
# both packages are uninstalled.

import os
import opk, cfg, opkgcl

opk.regress_init()
o = opk.OpkGroup()

dir = "dir10461"
filename1 = os.path.join(dir, "myfile1")
filename2 = os.path.join(dir, "myfile2")

os.makedirs(dir)
open(filename1, "w").close()
a = opk.Opk(Package="a")
a.write(data_files=[dir])
o.addOpk(a)

os.unlink(filename1)

open(filename2, "w").close()
b = opk.Opk(Package="b")
b.write(data_files=[dir])
o.addOpk(b)

os.unlink(filename2)
os.rmdir(dir)

o.write_list()
opkgcl.update()

opkgcl.install("b")
if not opkgcl.is_installed("b"):
     opk.fail("Package 'b' installed but reports as not installed.")

opkgcl.install("a")
if not opkgcl.is_installed("a"):
     opk.fail("Package 'a' installed but reports as not installed.")

opkgcl.remove("b")
opkgcl.remove("a")


if os.path.exists("{}/{}".format(cfg.offline_root, dir)):
    opk.fail("Dir '{}' incorrectly orphaned.".format(dir))
