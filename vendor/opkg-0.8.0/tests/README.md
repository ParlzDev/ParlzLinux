# Opkg Tests

This directory contains the Opkg integrated test suite.

This test framework detects errors in the functional capabilities of an opkg binary (the `core/` tests), and checks for known bugs (the `regress/` tests).

This framework is written in python and uses shell calls to interact with an `opkg` binary under-test. You can test a compiled `opkg` binary directly from the source tree (without installing it to your system root). The temporary data created by this framework are stored in your system `TMPDIR` and nothing should be installed to your system root.


----
## Running the Tests

The simplest way to run this test suite is to `cd` to the top-level directory and run `make check`. The `check` make-target will compile your changes (if necessary), and then run the whole suite of functional and regression tests against the compiled `opkg` binary.

Some regression tests are expected to fail when using either the internal satisfaction solver or libsolv; they will display as such when run. Please ensure that all tests are passing before submitting your changes to the mailing list.


### Test Configuration

The test framework uses configuration settings stored in the [`:tests/cfg.py`](/tests/cfg.py) file. By editing the values in that file, you can change the `TMPDIR` location that is used to host the framework's virtual workspace.

You can further tweak the subpaths that opkg uses for its data and configuration directories *within the test workspace*, by setting the `DATADIR`, `SYSCONFDIR`, and `VARDIR` environment variables during test execution. See the top-level `Makefile` (after automaking it) for a hint as to how those variable are normally defined.


### Running Single Tests

When debugging an issue or doing new development, it is sometimes helpful to run a single test case (or a subset of them) against your changes. The easiest way to do this is to overwrite the `REGRESSION_TESTS` variable like

```Make
REGRESSION_TESTS := test/you/care/about.py
```

Then just run `make check` again.


### Opkg Debug Output

The `opkgcl` module tests for an environment variable `DEBUG_OPKG_CMDS`. If it is set to "True", the `stderr` and `stdout` from opkg calls made during the test run will be printed to `stdout`.


----
## Authoring New Tests

If you need to author a new test, it is probably to reproduce and validate a discovered issue. Most test cases follow the workflow below, and it is a good idea to use the existing tests as a reference for how to accomplish common steps.

1. Create a new test case file under the `regress/` directory, named with the bugzilla issue number associated with the issue.

2. Start your test file with the common opkg issue template below. Filling this out will help others to understand what your test case is trying to reproduce, and also serve as a guide for you when implementing the test.

```
#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Reporter: $the_bug_reporter_contact_info
#
# What steps will reproduce the problem?
########################################
#
# #TODO
#
# What is the expected output? What do you see instead?
########################################
# Expected:
#    #TODO
# Actual:
#    #TODO
```

3. Import the `opk` and `opkgcl` python libraries, which provide utility functions for creating dummy packages and feeds, and instrumenting `opkg` binary calls respectively. Optionally, you may also wish to import the `cfg` module, if you need to access the paths defined within it during your test.

	```python
	import cfg, opk, opkgcl
	```

4. Run the `opk.regress_init()` method to tell the framework that you are starting a new test case.

5. Use the `opk` module to create a new `OpkGroup` (a package feed), and populate it with Packages that will reproduce your issue.

6. Call `opkgcl.update()` to load your dummy package group's index into the test framework's opkg environment. Then use the module to exercise the `opkg` calls which will reproduce your issue.

7. Use the `opk.fail()` method to fail the test when you have detected that the bug reproduces.


----
## Layout

* `cfg.py` : Defines test framework configuration variables.
* `opk.py` : Defines a helper module for constructing dummy packages and package feeds, for use by the testing framework.
* `opkgcl.py` : Defines a wrapper module for interacting with a compiled opkg binary; either installed to the system PATH or from the source tree.
* `core/` : Test cases which exercise core opkg functionality.
* `regress/` : Test cases for exercising abnormal behavior identified in bugs.