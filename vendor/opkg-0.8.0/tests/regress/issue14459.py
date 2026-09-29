#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Reporter: Rasmus Villemoes <rasmus.villemoes@prevas.dk>
#
# What steps will reproduce the problem?
########################################
#
# 1. pkg 'a' recommends pkg 'dne'
# 2. pkg 'dne' does not exist in the feed
# 3. ask opkg to `install a dne`
#
# What is the expected output? What do you see instead?
########################################
# Expected:
#  The opkg install operation should fail, and opkg should report that it cannot
#  find a package to satisfy 'dne'.
# Actual:
#  The opkg install operation succeeds, with no indication that package 'dne' is
#  not satisfiable.

import os
import opk, cfg, opkgcl

opk.regress_init()
o = opk.OpkGroup()

o.add(Package='a', Recommends='dne')
o.write_opk()
o.write_list()

opkgcl.update()

rc, stdout = opkgcl.opkgcl('install a dne')

assert rc != 0, \
    "Opkg install returned code 0; but package 'dne' should not be satisfiable."
