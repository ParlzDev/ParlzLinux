#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#

import os
import opk, cfg, opkgcl

opk.regress_init()

o = opk.OpkGroup()

# Create a package with a test file
f = open("test_file", "w")
f.write("Test")
f.close()
pkg = opk.Opk(Package="a", Version="1.0")
pkg.write(data_files=["test_file"])
o.addOpk(pkg)
o.write_list()
os.unlink("test_file")

opkgcl.update()

# Install the package
opkgcl.install("a")
if not opkgcl.is_installed("a"):
    opk.fail("Package 'a' installed but reports as not installed.")
if not os.path.exists("%s/test_file" % cfg.offline_root):
    opk.fail("Package 'a' installed but file 'test_file' not created.")

# Search for a file in the package we added
try:
    results = opkgcl.search("*test_file*")
    if results != ["a - 1.0"]:
        opk.fail("search for file did not return 'a' as its owner")
except FileNotFoundError as f:
    opkg.fail("search for file installed by 'a' failed")
except SystemExit as s:
    # pass along any opk.fail() in try body
    raise

# Search for a file that doesn't exist
try:
    results = opkgcl.search("*does_not_exist*")
    opkg.fail("search for nonexistant file did not return fatal error code")
except FileNotFoundError as f:
    # failure is expected
    pass
except SystemExit as s:
    # pass along any opk.fail() in try body
    raise
