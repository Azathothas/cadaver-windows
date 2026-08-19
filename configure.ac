dnl autoconf script for cadaver

dnl The version lives in the VERSION file, which Makefile.w32 reads as
dnl well, so the two build paths cannot report different numbers.
AC_INIT([cadaver],[m4_esyscmd_s([cat VERSION])],[],
        [],[https://github.com/Azathothas/cadaver-windows])

AC_PREREQ([2.69])

AC_COPYRIGHT([Copyright (c) 1999-2025 Joe Orton and others
This configure script may be copied, distributed and modified under the
terms of the GNU General Public license; see COPYING for more details])

AC_CONFIG_SRCDIR(src/cadaver.c)
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4 neon/macros])

dnl $host_os is what the platform check below reads, and nothing else
dnl in this script or in neon's macros sets it: without this it was
dnl empty, so the Windows branch never ran and the autotools build
dnl looked like a POSIX one on Windows.
AC_CANONICAL_HOST

AC_USE_SYSTEM_EXTENSIONS

AC_DEFINE([_GNU_SOURCE], 1, [Define to enable GNU extensions])

AC_PROG_CC
AC_PROG_INSTALL
AC_LANG([C])
AC_PROG_MAKE_SET
AC_PROG_RANLIB

PKG_PROG_PKG_CONFIG

AC_ARG_ENABLE(debugging,
AS_HELP_STRING([--disable-debugging], [disable runtime debugging messages]),,
    enable_debug=yes)

if test "$enable_debug" = "yes"; then
   AC_DEFINE(NE_DEBUGGING, 1, [Define to enable debugging])
fi

CAD_CHECK_READLINE
CAD_CHECK_ICONV

AC_ARG_ENABLE(netrc,
AS_HELP_STRING([--disable-netrc],[disable .netrc support]),,
    enable_netrc=yes)

NEON_WITH_LIBS

m4_ifndef([NE_MINIMUM_VERSION],
        [AC_FATAL([NE_MINIMUM_VERSION macro from neon>=0.34.x required to build.])])

NE_MINIMUM_VERSION(0, 31)

dnl Don't enable zlib or ACL support in neon
NEON_WITHOUT_ZLIB
NEON_WITHOUT_ACL

AC_PROVIDE([NE_SNPRINTF])

NEON_VPATH_BUNDLED(['$(top_srcdir)/neon/src'], [neon/src], [
 NEON_NORMAL_BUILD
 NEON_XML_PARSER
 TARGET_DEPS=neon/src/libneon.a
])

AC_SUBST(TARGET_DEPS)
AC_SUBST(RESOBJ)

NE_FORMAT_TIMET

if test "$enable_netrc" = "yes"; then
   LIBOBJS="$LIBOBJS lib/netrc.o"
   AC_DEFINE(ENABLE_NETRC, 1, [Define to enable netrc support])
   netrc_msg=enabled
else
   netrc_msg=disabled
fi

dnl getopt_long, mkstemp, strcasecmp and snprintf are all provided by
dnl mingw-w64's runtime as well as by every Unix libc cadaver has ever
dnl built on, so they are required rather than substituted.
AC_CHECK_FUNC(getopt_long,,
  [AC_MSG_ERROR([getopt_long is required])])
AC_CHECK_FUNC(strcasecmp,,
  [AC_MSG_ERROR([strcasecmp is required])])
dnl NEON_REPLACE_SNPRINTF would add lib/snprintf to LIBOBJS, which this
dnl tree no longer carries.  AC_PROVIDE above keeps neon from bringing
dnl its own copy in either.
AC_CHECK_FUNC(snprintf,,
  [AC_MSG_ERROR([snprintf is required])])

AC_HEADER_DIRENT
AC_STRUCT_DIRENT_D_INO
AC_CHECK_MEMBERS([struct dirent.d_type],,,[[#include <dirent.h>]])

AC_TYPE_UNSIGNED_LONG_LONG_INT

AC_CHECK_HEADERS(sys/time.h fcntl.h pwd.h stdarg.h memory.h alloca.h \
                 termios.h termio.h sgtty.h locale.h langinfo.h)

AC_CHECK_FUNCS(strdup strerror memcpy strcoll tcsetattr getpass fchmod \
               strtoull gettimeofday secure_getenv setlocale mkstemp \
               nl_langinfo)

dnl lib/system.c holds everything that differs between Windows and
dnl POSIX and is built on both; only the libraries differ.
AC_MSG_CHECKING([for the host platform])
case $host_os in
  mingw*|msys*)
    cad_platform=windows
    LIBS="$LIBS -lws2_32"
    dnl The application manifest that makes UTF-8 the process code page.
    dnl Without windres there is no way to embed it, and the build falls
    dnl back to what it did before, which is the ANSI code page.
    AC_CHECK_TOOL([WINDRES], [windres], [no])
    if test "$WINDRES" != "no"; then
       RESOBJ=win32/cadaver.res.o
    else
       AC_MSG_WARN([windres not found: building without the application
manifest, so a path outside the active ANSI code page will not work])
    fi
    ;;
  *)
    cad_platform=posix
    ;;
esac
AC_MSG_RESULT([$cad_platform])

NEON_WARNINGS

# Pick up config.h etc; and before any other paths.
CPPFLAGS="-I\$(top_builddir) -I\$(top_srcdir)/lib $CPPFLAGS"

AC_CONFIG_FILES([Makefile neon/src/Makefile])
AC_CONFIG_COMMANDS([source-directories],
  [for f in lib src win32; do test -d $f || mkdir $f; done])

AC_OUTPUT

AC_MSG_NOTICE([Configured to build AC_PACKAGE_STRING:

  Install prefix:        ${prefix}
  Compiler:              ${CC}
  neon library:          ${neon_library_message}
  XML parser:            ${neon_xml_parser_message}
  SSL library:           ${ne_SSL_message}
  Character conversion:  ${msg_iconv}
  GNU readline support:  ${msg_readline}
  .netrc support:        ${netrc_msg}
])

# For VPATH builds
for d in neon neon/src lib src win32; do
    test -d $d || mkdir -p $d
done
