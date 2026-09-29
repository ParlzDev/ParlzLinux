# OPKG Package Manager

Opkg is a lightweight distribution package manager, optimized for embedded applications and static image generation.

This repo is part of [The Yocto Project](https://www.yoctoproject.org/), which uses Opkg as the default package manager in [OpenEmbedded](https://www.openembedded.org/wiki/Main_Page). 

This project is a sibling to the [OpenWRT/opkg](https://openwrt.org/docs/guide-user/additional-software/opkg) project, and both are descendants of the discontinued [Itsy Package Manager (ipkg)](https://en.wikipedia.org/wiki/Ipkg).


* Bug Tracker: https://bugzilla.yoctoproject.org/buglist.cgi?product=opkg
* Mailing List: https://lists.yoctoproject.org/g/opkg
* Git Repository: https://git.yoctoproject.org/opkg/


----
## Getting Opkg

If you are using a distribution built by OpenEmbedded, your distribution vendor may have installed Opkg to your system. Try calling `opkg --help` from your command line.

Otherwise, you'll need to compile your own version of opkg from the sources in this repo. Follow the steps in the [`CONTRIBUTING.md`](/docs/CONTRIBUTING.md) "Building Opkg" section to generate the binaries.


----
## Using Opkg

Opkg is intentionally designed to resemble (and be largely compatible with) the Debian project's [dpkg](https://en.wikipedia.org/wiki/Dpkg) manager. Once you have `opkg` installed to your system and a valid opkg configuration, you can manage packages on your system with commands like...

```bash
opkg install /my-package.ipk  # to install a package from disk
opkg remove my-package  # to remove a package

opkg update  # to fetch the latest index of a remote package feed
opkg install remote-package  # to install a package from a remote feed
opkg upgrade remote-package  # to upgrade an installed package
# et cetera

opkg --help  # for more information
```


----
## Contributing to Opkg

See the [`CONTRIBUTING`](/docs/CONTRIBUTING.md) guide and other developer documentation in the `:docs/` directory, for information on how to build, test, and upstream your changes to this project. Thanks for your help!

----
## License

This project is licensed under GPL v2. See [`COPYING`](/COPYING) for the license text.