dnl configure.ac for serviceffmpeg
dnl Pure FFmpeg Enigma2 media service - replaces servicemp3
dnl Target: scarthgap (OpenPLi 9.x), Python3, no GStreamer

AC_INIT([serviceffmpeg],[1.0.0])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
LT_INIT

AC_PROG_CXX
AC_PROG_CC
AC_PROG_CXX
AM_PATH_PYTHON
AX_PYTHON_DEVEL
AC_LANG(C++)
AC_PROG_INSTALL

dnl LT_INIT MUST be here - defines LIBTOOL for Makefile.am
LT_INIT

dnl Explicit pkg-config check before any PKG_CHECK_MODULES
PKG_PROG_PKG_CONFIG

dnl -----------------------------------------------------------------------
dnl Enigma2 headers
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES([ENIGMA2], [enigma2])

dnl -----------------------------------------------------------------------
dnl FFmpeg (all four libs in one shot)
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
dnl Optional: ALSA (software audio fallback)
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
echo "  libass:  $have_libass"
echo "  ALSA:    $have_alsa"
echo "  player:  $player_bin"
echo ""
