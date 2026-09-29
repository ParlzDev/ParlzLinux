# Process this file with autoconf to produce a configure script
AC_INIT([opkg],[0.8.0])
AC_CONFIG_SRCDIR([libopkg/pkg.c])

AC_CONFIG_AUX_DIR([conf])
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([ -Werror foreign subdir-objects])
AC_CONFIG_HEADERS(config.h)

AC_CANONICAL_HOST
AC_USE_SYSTEM_EXTENSIONS

# Disable C++/Fortran checks
define([AC_LIBTOOL_LANG_CXX_CONFIG], [:])
define([AC_LIBTOOL_LANG_F77_CONFIG], [:])


for top_builddir in . .. ../.. $ac_auxdir $ac_auxdir/..; do
  test -f $top_builddir/configure && break
done

# large file support can be useful for gpgme
AC_SYS_LARGEFILE

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# The current libopkg API is deprecated so only build it if requested
AC_ARG_ENABLE(libopkg-api,
	      AS_HELP_STRING([--enable-libopkg-api],[Enable deprecated libopkg
			      API. [[default=no]] ]),
	      [want_libopkg_api="$enableval"], [want_libopkg_api="no"])
AM_CONDITIONAL(HAVE_LIBOPKG_API, test "x$want_libopkg_api" = "xyes")

# Checks for programs
AC_PROG_AWK
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AM_PROG_INSTALL_STRIP
LT_INIT
PKG_PROG_PKG_CONFIG([0.20])

# Checks for libraries

AC_ARG_WITH(static-libopkg,
[  --with-static-libopkg   Statically link libopkg into opkg.],
[static_libopkg="${withval}"])
AM_CONDITIONAL(STATIC_LIBOPKG, test "x$static_libopkg" = "xyes")

# Require libarchive
if test "$static_libopkg" = "yes"; then
  PKG_CHECK_MODULES_STATIC([LIBARCHIVE], [libarchive])
else
  PKG_CHECK_MODULES([LIBARCHIVE], [libarchive])
fi

# check for xz
AC_ARG_ENABLE(xz,
              AS_HELP_STRING([--enable-xz],[Enable xz compressor support
      [default=yes] ]),
    [want_xz="$enableval"], [want_xz="yes"])

if test "$want_xz" = "yes"; then
  AC_DEFINE(HAVE_XZ, 1, [Define if you want xz support])
fi
AM_CONDITIONAL(HAVE_XZ, test "$want_xz" = "yes")

# check for bzip2
AC_ARG_ENABLE(bzip2,
              AS_HELP_STRING([--enable-bzip2],[Enable bzip2 compressor support
      [default=no] ]),
    [want_bzip2="$enableval"], [want_bzip2="no"])

if test "$want_bzip2" = "yes"; then
  AC_DEFINE(HAVE_BZIP2, 1, [Define if you want bzip2 support])
fi
AM_CONDITIONAL(HAVE_BZIP2, test "$want_bzip2" = "yes")

# check for lz4
AC_ARG_ENABLE(lz4,
              AS_HELP_STRING([--enable-lz4],[Enable lz4 compressor support
      [default=no] ]),
    [want_lz4="$enableval"], [want_lz4="no"])

if test "$want_lz4" = "yes"; then
  AC_DEFINE(HAVE_LZ4, 1, [Define if you want lz4 support])
fi
AM_CONDITIONAL(HAVE_LZ4, test "$want_lz4" = "yes")

# check for zstd
AC_ARG_ENABLE(zstd,
              AS_HELP_STRING([--enable-zstd],[Enable zstd compressor support
      [default=no] ]),
    [want_zstd="$enableval"], [want_zstd="no"])

if test "$want_zstd" = "yes"; then
  AC_DEFINE(HAVE_ZSTD, 1, [Define if you want zstd support])
fi
AM_CONDITIONAL(HAVE_ZSTD, test "$want_zstd" = "yes")

# check for libcurl
AC_ARG_ENABLE(curl,
              AS_HELP_STRING([--enable-curl],[Enable downloading with curl
      [[default=yes]] ]),
    [want_curl="$enableval"], [want_curl="yes"])

if test "x$want_curl" = "xyes"; then
  PKG_CHECK_MODULES(CURL, [libcurl])
  AC_DEFINE(HAVE_CURL, 1, [Define if you want CURL support])
fi
AM_CONDITIONAL(HAVE_CURL, test "x$want_curl" = "xyes")

# check for sha256
AC_ARG_ENABLE(sha256,
              AS_HELP_STRING([--enable-sha256],[Enable sha256sum check
      [[default=no]] ]),
    [want_sha256="$enableval"], [want_sha256="no"])

if test "x$want_sha256" = "xyes"; then
  AC_DEFINE(HAVE_SHA256, 1, [Define if you want sha256 support])
fi
AM_CONDITIONAL(HAVE_SHA256, test "x$want_sha256" = "xyes")

# check for libssl-curl
AC_ARG_ENABLE(ssl-curl,
              AS_HELP_STRING([--enable-ssl-curl],[Enable certificate authentication with curl
                              [[default="yes"]] ]),
                              [want_sslcurl="$enableval"], [want_sslcurl="yes"])

if test "x$want_curl" = "xyes" -a "x$want_sslcurl" = "xyes"; then
  AC_DEFINE(HAVE_CURL, 1, [Define if you want CURL support])
  AC_DEFINE(HAVE_SSLCURL, 1, [Define if you want certificate authentication with curl])
  NEED_SSL_LIBS="yes"
fi

if test "x$NEED_SSL_LIBS" = "xyes"; then
  AC_MSG_CHECKING([if openssl is available])

  PKG_CHECK_MODULES(OPENSSL, openssl, [:], [:])
  if test "x$OPENSSL_LIBS" != "x"; then
          AC_MSG_RESULT(yes)
  else
          OPENSSL_LIBS="-lcrypto -lssl"
          dnl If pkg-config fails, run compile/link test.
          AC_LINK_IFELSE([AC_LANG_PROGRAM([[
#include <openssl/opensslv.h>
]], [[
return OPENSSL_VERSION_NUMBER; ]])],[
            AC_MSG_RESULT(yes)

          ],[
            AC_MSG_RESULT(no)
            AC_MSG_ERROR(OpenSSL not found)
          ])
  fi
  AC_SUBST(OPENSSL_LIBS)
fi

# check for ACL support
AC_ARG_WITH([acl], [AS_HELP_STRING([--with-acl], [Enable ACL support])])
if test "x$with_acl" = "xyes"; then
  AC_DEFINE([ENABLE_ACL], [1], [Enable ACL support])
fi

# check for xattr support
AC_ARG_WITH([xattr], [AS_HELP_STRING([--with-xattr], [Enable xattr support])])
if test "x$with_xattr" = "xyes"; then
  AC_DEFINE([ENABLE_XATTR], [1], [Enable xattr support])
fi

# check for libsolv solver
AC_ARG_WITH(libsolv, AS_HELP_STRING([--with-libsolv],[Use libsolv solver support.
  ]), [], [with_libsolv="no"])

if test "x$with_libsolv" != "xno"; then
    # do not allow opkg to be built with both old API and external solver support
    if test "x$want_libopkg_api" = "xyes"; then
        AC_MSG_ERROR(External solver support not compatible with deprecated libopkg API!)
    fi

    AC_MSG_CHECKING([for solver])
    AC_MSG_RESULT(libsolv)
    PKG_CHECK_MODULES(SOLVER, [libsolv >= 0.7.14])
    AC_DEFINE(HAVE_SOLVER_LIBSOLV,1,[Define if you want to use libsolv])
    AC_SUBST(SOLVER_CFLAGS)
    AC_SUBST(SOLVER_LIBS)
else
    AC_MSG_WARN([The opkg internal sat-solver is marked for DEPRECATION in a future release. Consider using the libsolv solver ("--with-libsolv") instead.])
    AC_DEFINE(HAVE_SOLVER_INTERNAL,1,[Define if you want to use the internal solver])
fi

AM_CONDITIONAL(HAVE_SOLVER_LIBSOLV, test "x$with_libsolv" != "xno")
AM_CONDITIONAL(HAVE_SOLVER_INTERNAL, test "x$with_libsolv" = "xno")

dnl **********
dnl GPGME
dnl **********

AC_ARG_ENABLE(gpg,
    AS_HELP_STRING([--enable-gpg],[Enable signature checking with gpgme
      [[default=yes]] ]),
    [want_gpgme="$enableval"], [want_gpgme="yes"])

if test "x$want_gpgme" = "xyes"; then
  ok="no"
  min_gpgme_version=1.0.0

  # Try to find gpgpme using pkg-config
  PKG_CHECK_MODULES([GPGME], [gpgme >= $min_gpgme_version], [ok="yes"],
                    [ok="no"])

  # If pkg-config failed, try the old way
  if test "$ok" = "no"; then
    AM_PATH_GPGME([$min_gpgme_version], [ok="yes"], [ok="no"])
  fi

  if test $ok = "yes"; then
    AC_MSG_RESULT(yes)
    AC_DEFINE(HAVE_GPGME, 1, [Define if you want GPG support])
  else
    AC_MSG_ERROR(GPGME $min_gpgme_version or later needed)
  fi

  # Try to find gpg-error using pkg-config
  PKG_CHECK_MODULES([GPGERR], [gpg-error], [ok="yes"], [ok="no"])

  # If pkg-config failed, try the old way
  if test "$ok" = "no"; then
    AC_PATH_PROG(GPGERR_CONFIG, gpg-error-config, "failed")
    if test $GPGERR_CONFIG != "failed" ; then
        GPGERR_CFLAGS=`$GPGERR_CONFIG --cflags`
        GPGERR_LIBS=`$GPGERR_CONFIG --libs`
    else
        AC_MSG_ERROR(GPGERR is required to build gpgme support)
    fi
  fi
fi
AM_CONDITIONAL(HAVE_GPGME, test "x$want_gpgme" = "xyes")

AC_SUBST(GPGME_CFLAGS)
AC_SUBST(GPGME_LIBS)
AC_SUBST(GPGERR_CFLAGS)
AC_SUBST(GPGERR_LIBS)

# Checks for header files
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([errno.h fcntl.h memory.h regex.h stddef.h stdlib.h string.h strings.h unistd.h utime.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_UID_T
AC_TYPE_MODE_T
AC_TYPE_OFF_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_CHECK_MEMBERS([struct stat.st_rdev])

# Checks endianness
AC_C_BIGENDIAN(BIGENDIAN_CFLAGS="-DWORDS_BIGENDIAN=1",)
AC_SUBST(BIGENDIAN_CFLAGS)

# Checks for library functions
AC_FUNC_CHOWN
AC_FUNC_FORK

AC_FUNC_UTIME_NULL
AC_FUNC_VPRINTF
AC_CHECK_FUNCS([memmove memset mkdir regcomp strchr strcspn strdup strerror strndup strrchr strstr strtol strtoul sysinfo utime])

if ! test -z "$SOURCE_DATE_EPOCH" ; then
    CLEAN_DATE=`LC_ALL=C date -d @$SOURCE_DATE_EPOCH +"%B %Y" | tr -d '\n'`
else
    CLEAN_DATE=`date +"%B %Y" | tr -d '\n'`
fi

AC_SUBST([CLEAN_DATE])

AC_CONFIG_FILES([Makefile
    libopkg/Makefile
    src/Makefile
    utils/Makefile
    libopkg.pc
    man/Makefile
    man/opkg.1
    man/opkg-key.1
    man/opkg.conf.5
    ])
AC_OUTPUT

# Issue this warning at the end so the user is likely to see it
if test "x$want_libopkg_api" = "xyes"; then
  AC_MSG_WARN(libopkg API is deprecated!)
fi
