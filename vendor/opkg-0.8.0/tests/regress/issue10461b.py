#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# 1.- Create package 'a', 'b' and 'c', which install files to
#     a shared new directory.
# 2.- Install package 'b', followed by packages 'a' and 'c'.
# 3.- Remove package 'c', followed by packages 'b' and 'a'.
#
# The shared new directory should be deleted only when the last package
# 'a' is uninstalled.
#
# 4. Install and remove package 'c' again
#
# The new directory should be deleted correctly after 'c' is uninstalled.
#


import os
import opk, cfg, opkgcl

opk.regress_init()
o = opk.OpkGroup()

dir = "dir10461"

os.makedirs(dir)
a = opk.Opk(Package="a")
a.write(data_files=[dir])
o.addOpk(a)

b = opk.Opk(Package="b")
b.write(data_files=[dir])
o.addOpk(b)

c = opk.Opk(Package="c")
c.write(data_files=[dir])
o.addOpk(c)

os.rmdir(dir)

o.write_list()
opkgcl.update()

opkgcl.install("b")
if not opkgcl.is_installed("b"):
     opk.fail("Package 'b' installed but reports as not installed.")

opkgcl.install("a")
if not opkgcl.is_installed("a"):
     opk.fail("Package 'a' installed but reports as not installed.")

opkgcl.install("c")
if not opkgcl.is_installed("c"):
     opk.fail("Package 'c' installed but reports as not installed.")

opkgcl.remove("c")
if not os.path.exists("{}/{}".format(cfg.offline_root, dir)):
    opk.fail("Dir '{}' incorrectly deleted.".format(dir))

opkgcl.remove("b")
if not os.path.exists("{}/{}".format(cfg.offline_root, dir)):
    opk.fail("Dir '{}' incorrectly deleted.".format(dir))

opkgcl.remove("a")
if os.path.exists("{}/{}".format(cfg.offline_root, dir)):
    opk.fail("Dir '{}' incorrectly orphaned.".format(dir))


opkgcl.install("c")
if not os.path.exists("{}/{}".format(cfg.offline_root, dir)):
    opk.fail("Dir '{}' not created by package 'c'.".format(dir))

opkgcl.remove("c")
if os.path.exists("{}/{}".format(cfg.offline_root, dir)):
    opk.fail("Dir '{}' incorrectly orphaned.".format(dir))