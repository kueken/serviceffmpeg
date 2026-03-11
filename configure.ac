AC_INIT([serviceffmpeg],[2.0.0])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

AC_PROG_CXX
AC_PROG_CC
AC_PROG_CXX
AM_PATH_PYTHON
AX_PYTHON_DEVEL
AC_LANG(C++)
AC_PROG_INSTALL
LT_INIT

PKG_PROG_PKG_CONFIG

dnl Enigma2
PKG_CHECK_MODULES([ENIGMA2], [enigma2])

dnl Python3 — cross-build safe
PKG_CHECK_MODULES([PYTHON], [python3-embed],
    [have_python=yes],
    [PKG_CHECK_MODULES([PYTHON], [python3],
        [have_python=yes], [have_python=no])])
AS_IF([test "x$have_python" = "xno"], [
    AC_PATH_PROG([PYTHON3_CONFIG], [python3-config])
    AS_IF([test -n "$PYTHON3_CONFIG"], [
        PYTHON_CFLAGS=`$PYTHON3_CONFIG --includes`
        PYTHON_LIBS=`$PYTHON3_CONFIG --ldflags --embed 2>/dev/null || $PYTHON3_CONFIG --ldflags`
        have_python=yes
    ])
])
AS_IF([test "x$have_python" = "xno"],
    [AC_MSG_ERROR([Python3 headers not found])])
AC_SUBST([PYTHON_CFLAGS])
AC_SUBST([PYTHON_LIBS])

dnl exteplayer3 binary path — runtime dep, compile-time path only for default
AC_ARG_WITH([exteplayer3],
    AS_HELP_STRING([--with-exteplayer3=PATH],
        [path to exteplayer3 binary @<:@default: BINDIR/exteplayer3@:>@]),
    [ep3_bin="$withval"],
    [ep3_bin="${bindir}/exteplayer3"])
AC_DEFINE_UNQUOTED([EXTEPLAYER3_BIN], ["$ep3_bin"],
    [Absolute path to exteplayer3 binary])
AC_DEFINE([DEBUG])

CXXFLAGS="$CXXFLAGS -Wall -Wno-deprecated -std=c++17"

AC_CONFIG_FILES([Makefile serviceffmpeg/Makefile plugin/Makefile])
AC_OUTPUT

AC_MSG_RESULT([
serviceffmpeg 2.0 (exteplayer3 backend)
  exteplayer3 : $ep3_bin
  Python      : $PYTHON_CFLAGS
])
