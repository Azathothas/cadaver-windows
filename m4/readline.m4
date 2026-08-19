dnl CAD_CHECK_READLINE looks for GNU readline and, if it is present,
dnl arranges for cadaver to use it for command-line editing, history and
dnl tab completion.  Without it cadaver falls back to the fgets() prompt
dnl in src/cadaver.c, which still works but edits nothing.
dnl
dnl readline needs a terminal library.  On a Unix system that is curses
dnl or ncurses; the MSYS2 mingw-w64 package is built against termcap and
dnl keeps history in a separate library, so all three are tried and the
dnl first one that satisfies the link wins.

AC_DEFUN([CAD_CHECK_READLINE], [

AC_ARG_ENABLE(readline,
	AS_HELP_STRING([--disable-readline], [disable readline support]),
	[use_readline=$enableval],
	[use_readline=yes])  dnl Defaults to ON (if found)

AS_IF([test "$use_readline" = "yes"], [
	dnl A terminal library first: readline references tputs and the
	dnl link fails without one.  None of them is fatal on its own.
	AC_SEARCH_LIBS([tputs], [termcap curses ncurses])

	AC_CHECK_LIB([readline], [readline],
		[LIBS="-lreadline $LIBS"
		 AC_DEFINE([HAVE_LIBREADLINE], [1],
		           [Define if you have the readline library])
		 msg_readline="enabled"],
		[msg_readline="not found"])

	AS_IF([test "$msg_readline" = "enabled"], [
		AC_SEARCH_LIBS([add_history], [history],
			[AC_DEFINE([HAVE_ADD_HISTORY], [1],
			           [Define if you have the add_history function])])

		AC_CHECK_HEADERS([history.h readline/history.h \
		                  readline.h readline/readline.h])

		dnl rl_completion_matches replaced completion_matches in
		dnl readline 4.2; src/cadaver.c defines the old name onto
		dnl the new one when this is missing.
		AC_CHECK_FUNCS([rl_completion_matches])
	])
], [
	msg_readline="disabled"
])])
