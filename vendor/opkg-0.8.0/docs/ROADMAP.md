# Opkg Roadmap

This document contains historic context for this project which developers and maintainers might be interested to know, as well as a prospectus for future development. This document is the domain of the current maintainer.

If you are interested in advancing any of the initiatives in this document, it would be a major help to the community and earn you a lot of good will. :)


----
## Future Development Initiatives

Entries in this section marked with **[1.0]** must be completed before we call Opkg version 1 "stable".


### General

* Convert the project to use build tooling which is more modern than autotools. It seems the only reason autotools was chosen for the initial implementation was that "it worked". But no one was an expert at it and the existing implementation is rather old and crusty. It pre-dates git and tends to leave clutter around, and doesn't understand modern project layouts. Investigate the modern ecology of C/C++ build tooling (start w/ cmake) and try to follow the winds to find something better.

* Publish a docker container which can build opkg. This is a nice-to-have for any modern project, since it can get developers onboarded with making and testing contributions quickly. Start by talking to Alex Stewart, who already has a container that he uses for his development.

* Publish a docker container which runs opkg. It is fairly common contemporarily to give users a one-button way to run your application in a sandbox and play around with its abilities. I think this would go a long way towards driving opkg adoption.


### libopkg

* Remove the 'XXX', 'FIXME', and 'TODO' comments in the code. If they're real objectives that should be fixed, we should create bugs for them, not talk about them in code where no one will see them.

* **[1.0]** Deprecate the internal sat solver implementation. `libsolv` has been the preferred backend for many years, to the extent that internal solver bugs aren't even prioritized on the bug backlog. Ask the community if there is *anyone* who is still using the internal solver, and move to remove it entirely and make libsolv the default.


### Testing

* Reimplement the test framework in a more modern test tool like [pytest](https://docs.pytest.org/en/7.2.x/).

* Develop an efficient way to test large (virtual) package feeds. Some issues (eg. https://bugzilla.yoctoproject.org/show_bug.cgi?id=13593) report poor opkg performance when using reasonably large package feeds. These issues are difficult to test and develop-for, because it is expensive in the current framework to generate thousands of packages on disk for a test case. However, there is no real reason that these files have to exist all the time. If we leveraged something like a [flask server](https://flask.palletsprojects.com/en/2.2.x/), we could generate a package index and thousands of "virtual packages" on-demand, and only deliver opkg the content that it requests.

* **[1.0]** Revisit the `:tests/core/` test cases. They have not seen new content in many years, and I (astewart) do not believe that they represent a decent set of coverage for the core functionality of opkg.

* Make the test framework work with both `opkg` and `dpkg` simultaneously. Many discussion threads in the opkg community start with asking "What does dpkg do in this case?". We should have easy tooling for testing arbitrary package setups in dpkg, and comparing opkg's functionality against it.

* **[1.0]** Stand up an auto-test suite. Opkg already has a reasonably reliable integrated test suite. There is no reason that the project shouldn't have an autotester somewhere which can test incoming patchsets. In fact, Alex Stewart already has a personal gitlab instance (https://gitlab.com/astewart.49c6/opkg) with an autotest pipeline that uses the free resources that GitLab provides.


### Documentation

* **[1.0]** Update the opkg `man` pages.

* **[1.0]** Document the Opkg API; it is a requirement for taking Opkg to an official version 1.0 release.
  
* **[1.0]** Publish the opkg man pages and API documentation to the internet somewhere. New users who are looking for help using opkg will likely find the [OpenWRT docs](https://openwrt.org/docs/guide-user/additional-software/opkg), which do not reflect how this fork operates.
