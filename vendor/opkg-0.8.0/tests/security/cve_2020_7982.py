#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# NVD: https://nvd.nist.gov/vuln/detail/CVE-2020-7982
# opkg-lede sec advisory: https://openwrt.org/advisory/2020-01-31-1
# Exploit walkthrough: https://www.mayhem.security/blog/uncovering-openwrt-remote-code-execution-cve-2020-7982
#
# An issue was discovered in OpenWrt 18.06.0 to 18.06.6 and 19.07.0, and LEDE 17.01.0 to 17.01.7. A bug in the fork of the opkg package manager before 2020-01-25 prevents correct parsing of embedded checksums in the signed repository index, allowing a man-in-the-middle attacker to inject arbitrary package payloads (which are installed without verification).
#

from random import randbytes
import os
import opk, cfg, opkgcl


def generate_random_file(filepath, size_bytes):
    with open(filepath, "wb") as fp_data:
        fp_data.write(randbytes(size_bytes))


opk.regress_init()
o = opk.OpkGroup()

# Generate a GOOD package containing random data
PKG_DATA_PATH = "cve_2020_7982.data"
with open(PKG_DATA_PATH, "wb") as fp_data:
        fp_data.write(randbytes(2048))
pkg_a = o.add(Package='a')
pkg_a_path = pkg_a.write(data_files=[PKG_DATA_PATH])
o.write_opk()

# Generate the package list using the good package.
o.write_list()
opkgcl.update()

# This exploit requires the attacker to change the affected package's contents, While retaining the overall file size. Since packages are compressed files, this is hard to do reliably. But we can simulate a successful attempt by simply creating a random file and truncating it to the size of the original IPK.
pkg_a_size = os.stat(pkg_a_path).st_size
with open(pkg_a_path, "wb") as fp_pkg:
    fp_pkg.write(randbytes(2048))
    fp_pkg.truncate(pkg_a_size)

# Now attempt to install the corrupted package from the feed. OPKG should detect the incorrect checksum and refuse to install it.
rc = opkgcl.install("a")
assert rc != 0
assert not opkgcl.is_installed("a"), "Opkg installed corrupted package; possible vulnerability to CVE-2020-7982."
