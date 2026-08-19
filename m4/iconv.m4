dnl CAD_CHECK_ICONV looks for iconv(), which cadaver uses to convert
dnl between the character encoding of the terminal and the UTF-8 that
dnl WebDAV puts on the wire.
dnl
dnl This replaces gettext's AM_ICONV, which is correct but drags in nine
dnl further macro files for the sake of one link test.  cadaver needs
dnl only two facts: whether <iconv.h> is usable, and which library the
dnl entry points live in.  GNU libiconv renames them through macros in
dnl the header, so the test has to go through the header rather than
dnl name iconv_open directly.

AC_DEFUN([CAD_CHECK_ICONV], [

AC_ARG_WITH(iconv,
	AS_HELP_STRING([--without-iconv],
	               [do not use iconv for character conversion]),
	[use_iconv=$withval],
	[use_iconv=yes])

msg_iconv="not used"

AS_IF([test "$use_iconv" != "no"], [
	AC_CHECK_HEADERS([iconv.h], [
		cad_iconv_save_LIBS=$LIBS
		cad_have_iconv=no
		for cad_lib in "" "-liconv"; do
			LIBS="$cad_lib $cad_iconv_save_LIBS"
			AC_MSG_CHECKING([for iconv_open${cad_lib:+ in $cad_lib}])
			AC_LINK_IFELSE([AC_LANG_PROGRAM(
				[[#include <stdlib.h>
#include <iconv.h>]],
				[[iconv_t cd = iconv_open("UTF-8", "UTF-8");
if (cd != (iconv_t)-1) iconv_close(cd);]])],
				[cad_have_iconv=yes], [cad_have_iconv=no])
			AC_MSG_RESULT([$cad_have_iconv])
			AS_IF([test $cad_have_iconv = yes], [
				AC_DEFINE([HAVE_ICONV], [1],
				          [Define if you have the iconv function])
				msg_iconv="enabled${cad_lib:+ ($cad_lib)}"
				break
			])
		done
		AS_IF([test $cad_have_iconv = no], [LIBS=$cad_iconv_save_LIBS])
	])
])])
