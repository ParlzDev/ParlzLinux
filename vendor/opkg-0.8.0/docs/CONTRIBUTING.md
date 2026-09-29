# Contributing to Opkg

Thanks for taking an interest in contributing to the [Yocto project's](https://www.yoctoproject.org/) fork of the Opkg package manager!

This document should provide useful information for how to get started developing, testing, and upstreaming to our mainline.


----
## Communication

Clone the **source** from the Yocto project's [git server](https://git.yoctoproject.org/opkg/), and read the [README](https://git.yoctoproject.org/opkg/tree/README).

Get **help** using this repository and **discuss** changes on the [opkg mailing list](https://lists.yoctoproject.org/g/opkg).

File **bugs** and **enhancement** requests to the Yocto project's [opkg bugzilla tracker](https://bugzilla.yoctoproject.org/buglist.cgi?quicksearch=Product%3Aopkg)

Send **security** concerns directly to the project **maintainer** by email: Alex Stewart <[alex.stewart@emerson.com](mailto:alex.stewart@emerson.com)>

**Historic Maintainers**
- Alejandro del Castillo <alejandro.delcastillo@ni.com>
- Paul Barker <paul@paulbarker.me.uk>
- Thomas Wood <thomas@openedhand.com>
- Tick Chen <ticktock35@gmail.com>
- Graham Gower <graham.gower@gmail.com>
- Ipkg
	- Pierluigi Frullani <pigi@frumar.it>
	- Carl Worth <cworth@handhelds.org>
	- Steve Ayer <steven.ayer@compaq.com>


----
## Building Opkg

1. Build the libsolv satisfaction solver.

[libsolv](https://github.com/openSUSE/libsolv) is a free package dependency solver, which opkg uses as its backend satisfaction solver. Though it is strictly possible to build and use opkg with its legacy "internal" solver instead, it is not well-maintained and not recommended.

You will need the libsolv development headers to build opkg. If they are not available from your distribution vendor, you can quickly build and install libsolv using the following commands on debian/ubuntu.

```bash
apt-get update && apt-get install cmake g++ git zlib1g-dev
git clone https://github.com/openSUSE/libsolv.git /usr/local/libsolv
cd /usr/local/libsolv
mkdir build && cd build
cmake \
	-DCMAKE_INSTALL_PREFIX="/usr/local/" \
	-DENABLE_COMPLEX_DEPS=on \
	-DMULTI_SEMANTICS=on \
	..
make install
ldconfig
```

**NOTE:** The libsolv `ENABLE_COMPLEX_DEPS` and `MULTI_SEMANTICS` options must be enabled for libsolv to support opkg.

2. Install opkg build requirements.

**Debian 11**
```bash
apt-get update && apt-get install -y autoconf automake git libarchive-dev libcurl4-openssl-dev libgpgme11-dev libssl-dev libtool-bin make pkg-config python3 shtool
```

3. Clone the repository from the Yocto project git server. The use autoconf to configure, and gnu make to build the project.

```bash
git clone https://git.yoctoproject.org/opkg --branch master
cd opkg
autoconf
./configure --with-libsolv
make
```

For more information about configuring projects with autoconf, please reference the [autoconf documentation](https://www.gnu.org/software/autoconf/manual/autoconf-2.71/html_node/Basic-Installation.html).

4. (Optional) Install opkg to your system.

It is not required that you install opkg to test your changes, but you can do so with the following steps.

```bash
./configure --with-libsolv --prefix=/usr/local
# or some other installation --prefix
make install
# verify:
opkg --version
```


----
## Testing

Once you have *built* your change, you can validate your changes using the integrated opkg test suite, in the `:tests/` directory.

See the [`:tests/README.md`](/tests/README.md) document for information about how to run the test suite and author new tests. (tldr; run `make check`)


----
## Commit Guidelines

This project uses *commits* as the fundamental unit-of-change (not pull requests). So please follow common best practices for authoring your commits such that they are: atomic, comprehensible, and considerate of users who work in the embedded space.

**Commit Signing-offs.** Please add a signoff to each of your commits, using the `--signoff` argument to git. (eg. `git commit -s`)

**Bug Fixes.** If your change resolves a bug from the opkg bugzilla, please include a `Closes: ${bugzilla number}` trailer to your commit message.

```bash
git commit -s --trailer Closes=12345
```


----
## Submitting Changes Upstream

**Git Send-Email.** Once you have *tested* your change, you can submit it to the opkg mainline by embedding it into an email, and sending it to the opkg mailing list (<opkg@lists.yoctoproject.org>). When you do, please prefix your email with the tags: `[opkg][PATCH]`.

The easiest way to do this is using the git send-email extension. You can use the following commands to configure your opkg workspace with the correct defaults.

```bash
git config diff.renames copy
git config format.to "opkg@lists.yoctoproject.org"
git config format.subjectprefix "opkg][PATCH"
```

You can then create a patchset using the `git format-patch` command, and send it upstream. eg.

```bash
git format-patch origin/master..HEAD
# or
git format-patch --cover-letter origin/master..HEAD

git send-email ./*.patch
```


----
## Developer's Certificate of Origin

Developer's Certificate of Origin 1.1

By making a contribution to this project, I certify that:

(a) The contribution was created in whole or in part by me and I
	have the right to submit it under the open source license
	indicated in the file; or

(b) The contribution is based upon previous work that, to the best
	of my knowledge, is covered under an appropriate open source
	license and I have the right under that license to submit that
	work with modifications, whether created in whole or in part
	by me, under the same open source license (unless I am
	permitted to submit under a different license), as indicated
	in the file; or

(c) The contribution was provided directly to me by some other
	person who certified (a), (b) or (c) and I have not modified
	it.

(d) I understand and agree that this project and the contribution
	are public and that a record of the contribution (including all
	personal information I submit with it, including my sign-off) is
	maintained indefinitely and may be redistributed consistent with
	this project or the open source license(s) involved.
