#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Create package `a` which recommends `b`
# Install `a`
# Pkg 'a' and 'b' should have be installed
# No error occurs when trying to install 'b' again.

import os
import opk, cfg, opkgcl

opk.regress_init()
o = opk.OpkGroup()

o.add(Package='a', Recommends='b')
o.add(Package='b')
o.write_opk()
o.write_list()

opkgcl.update()

rc, _ = opkgcl.opkgcl('install a')

assert rc == 0, "Opkg failed when installing 'a'"

if not opkgcl.is_installed('a'):
    opk.fail("Package 'a' failed to install.")

if not opkgcl.is_installed('b'):
    opk.fail("Package 'b' (recommended by 'a') failed to install.")

rc, _ = opkgcl.opkgcl('install b')

assert rc == 0, "Opkg failed when installing 'b'"
