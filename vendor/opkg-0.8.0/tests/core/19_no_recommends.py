#! /usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# 1. Create packages 'a', 'b' which recommends 'a' and 'c' which depends on 'a'.
#    Install 'b' with '--no-install-recommends' and ensure 'a' is not installed.
#    Install 'c' with '--no-install-recommends' and ensure 'a' is installed.
# 2. Repeat the same test but enable no_install_recommends via configuration file.
# 3. Install 'b' with --install-recommends to verify that the configuration option
#    can be overridden.

import os
import opk, cfg, opkgcl


def enable_no_install_recommends_config_option():
    sysconfdir=os.environ['SYSCONFDIR']
    testconfdir=cfg.offline_root+sysconfdir
    with open('%s/opkg/opkg.conf' % testconfdir, "a") as f:
        f.write('option no_install_recommends 1\n')


def install_packages(expect_recommendations_installed, opkg_param=''):
    o = opk.OpkGroup()
    o.add(Package="a")
    o.add(Package="b", Recommends="a")
    o.add(Package="c", Depends="a")
    o.write_opk()
    o.write_list()

    opkgcl.update()

    opkgcl.install("b", opkg_param)
    if opkgcl.is_installed("a") != expect_recommendations_installed:
        opk.fail("Recommended package 'a' installed")

    opkgcl.install("c", opkg_param)
    if not opkgcl.is_installed("a"):
        opkg.fail("Dependent package 'a' is not installed")


opk.regress_init()
install_packages(expect_recommendations_installed=False, opkg_param='--no-install-recommends')

opk.regress_init()
enable_no_install_recommends_config_option()
install_packages(expect_recommendations_installed=False)

opk.regress_init()
enable_no_install_recommends_config_option()
install_packages(expect_recommendations_installed=True, opkg_param='--install-recommends')
