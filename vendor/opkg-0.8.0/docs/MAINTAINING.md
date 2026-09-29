# Maintaining Opkg

This document contains policy documentation and helpful references for how this fork of the Opkg project is maintained and released.


----
## Policies

### Version Policy

This fork follows the semantic versioning specification outlined [here](https://semver.org/spec/v2.0.0.html) as SemVer 2.0.

In short...
```
Given a version number MAJOR.MINOR.PATCH, increment the:

    1. MAJOR version when you make incompatible API changes
    2. MINOR version when you add functionality in a backwards compatible manner
    3. PATCH version when you make backwards compatible bug fixes

Additional labels for pre-release and build metadata are available as extensions to the MAJOR.MINOR.PATCH format.
```

**Public API.** For Opkg, the public API is the `opkg` CLI application; the opkg configuration files; the public API of the libopkg library; and the on-disk structure of OPK/IPK package files, status, and control files.


### Release Policy

This fork's maintainer will create a stable release on a semi-annual basis (in June and December) by tagging the ultimate release commit with a release tag and uploading a canonical archive of the release source to the yocto project's [opkg download mirror](http://downloads.yoctoproject.org/releases/opkg/). For more detailed information, see the "Releasing Opkg" section below.

If there have been no substantial changes to the project source since the prior release, the maintainer might choose to skip that release. This decision should ideally be announced on the mailing list.


----
## Releasing Opkg

### Requirements

To carry out the steps in this section, you must have:
1. **permission** from the Yocto project to act as maintainer (obviously)
	* If you believe it is appropriate for you to be a maintainer, start a discussion on the opkg mailing list (see CONTRIBUTING) and on the Yocto project mailing list <yocto@lists.yoctoproject.org>.

2. a consistent **GPG signing keypair**, for artifact signing.
	* Google around for how to make one.

3. a yocto project (Linux Foundation) **SSH account**, to upload the opkg release information.
	* If you do not have an account, contact Michael Halstead <mhalstead@linuxfoundation.org>.


### Workflow

1. Update the [CHANGELOG](CHANGELOG.md) with a summary of the notable changes since the prior release.

2. Based on the changes, set the version for the release in the `configure.ac` file. Bump the version numbers according to the Version Policy above. Commit the release version.

3. Create the release artifacts using the `:scripts/make-dist.sh` script.
	```bash
	bash scripts/make-dist.sh -k ${YOUR_GPG_PRIVATE_KEY} HEAD
	```

	The script should create a complete set of release artifacts in a `:dist/` directory.

4. (Optional) Test the dist build in an OpenEmbedded-Core build.

5. Tag and sign the release commit with the `${VERSION}` string.
	```bash
	git tag --sign --local-user=${GPG_KEY_ID} -m "Opkg release $VERSION" v${VERSION}
	```

6. Add the "`+git`" suffix to the version string in the `AC_INIT()` call in [`configure.ac`](configure.ac), to support next-cycle's development.

7. Push the `:dist/` artifacts to the yocto download server using SSH.
	```bash
	ssh ${USER}@access.yocto.io "mkdir -p ~/opkg; rm -v ~/opkg/*"
	rsync ./dist/* ${USER}@access.yocto.io:~/opkg
	```
	Notify Michael Halstead <mhalstead@linuxfoundation.org> that there is a new opkg release at that path which is ready to be published to the mirror.

8. Publish the release commits and tag to the [Yocto git server](https://git.yoctoproject.org/opkg/).
	```bash
	git push yocto HEAD:master
	git push yocto v${VERSION}
	```

9.  Make an announcement to the opkg mailing list that the new release is available. Include the release notes in the email.

10. Prepare an upgrade commit for the [OE-core opkg recipe](https://git.openembedded.org/openembedded-core/tree/meta/recipes-devtools/opkg). Once the source tar is available on the download mirror, upstream the recipe upgrade with the OE-core mailing list.

	```
	opkg: upgrade to version ${VERSION}

	Opkg ${VERSION} changes:
	<snip from the release log>

	${SIGNOFF}
	```

11. Add the released version to the enum of `versions` in the [opkg project config](https://bugzilla.yoctoproject.org/editversions.cgi?product=opkg) on bugzilla.