dnl configure.ac for serviceffmpeg
dnl Pure FFmpeg Enigma2 media service - replaces servicemp3
dnl Target: scarthgap (OpenPLi 9.x), Python3, no GStreamer

AC_INIT([serviceffmpeg],[1.0.0])
AM_INIT_AUTOMAKE([subdir-objects no-define])
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

dnl LT_INIT defines LIBTOOL - required for plugin_LTLIBRARIES
LT_INIT

dnl Explicit pkg-config
PKG_PROG_PKG_CONFIG

dnl -----------------------------------------------------------------------
dnl Python3 headers - needed for pythonmodule.cpp
dnl In OE cross-build: python3 headers are in STAGING_INCDIR/python3.x/
dnl We use pkg-config (python3-embed preferred, python3 as fallback)
dnl AX_PYTHON_DEVEL is NOT used - it runs a link test against the host
dnl which always fails in a cross-build environment.
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES([PYTHON], [python3-embed],
    [have_python=yes],
    [PKG_CHECK_MODULES([PYTHON], [python3],
        [have_python=yes],
        [have_python=no])])

if test "x$have_python" = "xno"; then
    dnl Last resort: try python3-config directly
    AC_PATH_PROG([PYTHON3_CONFIG], [python3-config])
    if test -n "$PYTHON3_CONFIG"; then
        PYTHON_CFLAGS=`$PYTHON3_CONFIG --includes`
        PYTHON_LIBS=`$PYTHON3_CONFIG --ldflags --embed 2>/dev/null || $PYTHON3_CONFIG --ldflags`
        have_python=yes
    fi
fi

if test "x$have_python" = "xno"; then
    AC_MSG_ERROR([Python3 development headers not found.
    In OE build: ensure python3-native is in DEPENDS and
    STAGING_INCDIR is passed via EXTRA_OECONF.])
fi

AC_SUBST([PYTHON_CFLAGS])
AC_SUBST([PYTHON_LIBS])

dnl -----------------------------------------------------------------------
dnl Enigma2 headers
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES([ENIGMA2], [enigma2])

dnl -----------------------------------------------------------------------
dnl FFmpeg
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES([FFMPEG], [libavformat libavcodec libavutil libswresample])

dnl -----------------------------------------------------------------------
dnl Optional: libass
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES([LIBASS], [libass],
    [AC_DEFINE([HAVE_LIBASS], [1], [libass available])
     have_libass=yes],
    [have_libass=no])
AM_CONDITIONAL([HAVE_LIBASS], [test "x$have_libass" = "xyes"])

dnl -----------------------------------------------------------------------
dnl Optional: ALSA
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES([ALSA], [alsa],
    [AC_DEFINE([HAVE_ALSA], [1], [ALSA available])
     have_alsa=yes],
    [have_alsa=no])
AM_CONDITIONAL([HAVE_ALSA], [test "x$have_alsa" = "xyes"])

dnl -----------------------------------------------------------------------
dnl ffmpeg-player binary path
dnl -----------------------------------------------------------------------
AC_ARG_WITH([player-bin],
    AS_HELP_STRING([--with-player-bin=PATH],
                   [Path for ffmpeg-player binary (default: BINDIR/ffmpeg-player)]),
    [player_bin="$withval"],
    [player_bin="${bindir}/ffmpeg-player"])
AC_DEFINE_UNQUOTED([SFMP_PLAYER_BIN], ["$player_bin"],
                   [Path to ffmpeg-player binary])

dnl -----------------------------------------------------------------------
dnl Compiler flags
dnl -----------------------------------------------------------------------
CXXFLAGS="$CXXFLAGS -Wall -Wno-deprecated -std=c++17"

AC_CONFIG_FILES([
    Makefile
    serviceffmpeg/Makefile
    plugin/Makefile
])
AC_OUTPUT

echo ""
echo "serviceffmpeg build configuration:"
echo "  Python CFLAGS: $PYTHON_CFLAGS"
echo "  libass:        $have_libass"
echo "  ALSA:          $have_alsa"
echo "  player binary: $player_bin"
echo ""
