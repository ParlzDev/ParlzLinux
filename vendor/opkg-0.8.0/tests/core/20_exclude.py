#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Create packages 'a', 'b' which recommends 'a' and 'c' which depends on 'a'.
# Install 'b' with '--add-exclude a' and ensure 'b' is installed but 'a' is not.
# Install 'c' with '--add-exclude a' and ensure 'c' and 'a' are not installed.
#

import os
import re
import opk, cfg, opkgcl

opk.regress_init()

_, version = opkgcl.opkgcl("--version")
libsolv = True if "libsolv" in version else False

o = opk.OpkGroup()
o.add(Package="a1")
o.add(Package="b", Recommends="a1, a2")
o.add(Package="c", Depends="a1")
o.add(Package="a2")
o.write_opk()
o.write_list()

opkgcl.update()

opkgcl.install("b", "--add-exclude a1")
if opkgcl.is_installed("a1"):
    opk.fail("Package 'a1' installed despite being excluded.")
if not opkgcl.is_installed("b"):
    opk.fail("Package 'b' not installed, excluded pacakge was only a recommendation.")
if not opkgcl.is_installed("a2"):
    opk.fail("Package 'a2' not installed, excluded pacakge was only a recommendation.")

opkgcl.remove("b a2", "--force-depends")

opkgcl.install("c", "--add-exclude a1")
if opkgcl.is_installed("a1"):
    opk.fail("Package 'a1' installed despite being excluded.")
if opkgcl.is_installed("c"):
    opk.fail("Package 'c' installed despite required package being excluded.")

# Re-do 'c' install, but capture stdout this time to make sure that we have a
# message indicating that 'a1' being excluded is preventing 'c' installation.
# The solvers indicate this case with different messages.
status, stdout = opkgcl.opkgcl("--force-postinstall install c --add-exclude a1")
if libsolv:
    if not re.search(r"package a1-([^ ]*) can only be installed by a direct request", stdout):
        opk.fail("[libsolv] Package 'c' install should indicate 'a1' only installable by direct request")
else:
    if not re.search(r"exclude required package a1 at users request", stdout):
        opk.fail("[internalsolv] Package 'c' install should indicate 'a1' was excluded")

opkgcl.install("b", "--add-exclude 'a*'")
if not opkgcl.is_installed("b"):
    opk.fail("Package 'b' not installed, excluded pacakge was only a recommendation.")
if opkgcl.is_installed("a1"):
    opk.fail("Package 'a1' installed despite being excluded.")
if opkgcl.is_installed("a2"):
    opk.fail("Package 'a2' installed despite being excluded.")
