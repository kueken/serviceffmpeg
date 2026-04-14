AC_INIT([serviceexteplayer3],[2.0.0])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
LT_INIT

AC_PROG_CC
AC_PROG_CXX
AM_PATH_PYTHON
AX_PYTHON_DEVEL
AC_LANG(C++)

PKG_PROG_PKG_CONFIG

dnl Enigma2
PKG_CHECK_MODULES([ENIGMA2], [enigma2])

dnl sigc++-3.0
PKG_CHECK_MODULES([SIGCPP], [sigc++-3.0])

dnl Python3
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

dnl FFmpeg — required for exteplayer3 built-in
PKG_CHECK_MODULES([FFMPEG],
    [libavformat libavcodec libavutil libswresample libswscale],
    [],
    [AC_MSG_ERROR([FFmpeg libraries not found])])

CXXFLAGS="$CXXFLAGS -Wall -Wno-deprecated -std=c++17"

AC_CONFIG_FILES([
    Makefile
    src/Makefile
    src/exteplayer3/Makefile
    serviceexteplayer3/Makefile
    plugin/Makefile
])
AC_OUTPUT

AC_MSG_RESULT([
serviceexteplayer3 2.0 (exteplayer3 built-in)
  Python  : $PYTHON_CFLAGS
  FFmpeg  : $FFMPEG_LIBS
])
