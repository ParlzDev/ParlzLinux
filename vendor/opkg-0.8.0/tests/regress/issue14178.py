#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Reporter: Kay Hackmack
#
# Due to an error on our side our feed's Packages/Packages.gz was generated
# like this (xyz is some package-specific data):
#
# Package: xyz
# Version: 20.7.0.79-0+d79
# Architecture: core2-64
# Maintainer: Example User <user@example.com>
# Source: xyz
# Section: Add-Ons
# Priority: extra
# Depends: xyz
# Homepage: https://urldefense.com/v3/__https://www.example.com/__;!!FbZ0ZwI3Qg!rQIEh4JM-KkE8EhjDYoIPcyuh3a_FnCKbMxd_rUY8l2hFZ6-vHs0sRM4r_iSUU58fYHXgj-I54BpJXMGFUl2vQ$
# DisplayVersion: 20.7
# DisplayName: xyz
# UserVisible:  yes
# Description: xyz
# xyz
# MD5Sum:
# Size:
# Filename: xyz.ipk
#
# After opkg on the target accessed/cached that feed it starts segfaulting
# when running any command.
# The key here seems to be that MD5Sum and/or Size: are empty. I'm not sure
# which entry (MD5Sum or Size) is causing the crash or both at the moment.

import os
import opk, cfg, opkgcl

BOGUS_SIZES = [
    "",                      # empty string
    "-1",                    # negative number
    "-9223372036854775808",  # LLONG_MIN - 1
    "18446744073709551616",  # ULLONG_MAX + 1
    "foo",                   # no digits
    "42fortytwo",            # digits but also (non-hex) digits
]

for bogosity in BOGUS_SIZES:
    opk.regress_init()

    o = opk.OpkGroup()
    o.add(Package="xyz", Version="1.0")
    o.write_opk()

    # write out a package list with bogus size
    with open('Packages', 'w') as f:
        for pkg in o.opk_list:
            for k in pkg.control.keys():
                f.write('{}: {}\n'.format(k, pkg.control[k]))
            fname = pkg.control['Filename']
            md5sum = opk.md5sum_file(fname)
            f.write('Size: {}\n'.format(bogosity))
            f.write('MD5Sum: {}\n'.format(md5sum))
            f.write('\n')

    if opkgcl.update() != 0:
        fail("failed to update package list")

    (status, output) = opkgcl.opkgcl('list')
    if status != 0:
        # Even with the ERROR printed this is still a success, in that
        # it should not be returning code 139 (SIGSEGV)
        opk.fail("opkg list returned nonzero exit code {}".format(status))
    if "Failed to parse Size line for xyz" not in output:
        opk.fail("expected to find error in output, but did not")

    (status, output) = opkgcl.opkgcl('info xyz')
    if status != 0:
        opk.fail("opkg list returned nonzero exit code {}".format(status))
    if "Failed to parse Size line for xyz" not in output:
        opk.fail("expected to find error in output, but did not")
