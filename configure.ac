dnl configure.ac for serviceffmpeg
dnl Pure FFmpeg Enigma2 media service - replaces servicemp3 and servicehisilicon
dnl Target: OpenEmbedded/Yocto cross-compile for mipsel, ARM, x86

AC_INIT([serviceffmpeg], [1.0.0])
AC_CONFIG_AUX_DIR([.])
AM_INIT_AUTOMAKE([foreign dist-bzip2 subdir-objects])
AM_CONFIG_HEADER(config.h)
AC_CONFIG_MACRO_DIR([m4])

AC_PROG_CXX
AC_PROG_CC
AC_PROG_LIBTOOL
AC_PROG_INSTALL

dnl -----------------------------------------------------------------------
dnl Check for required FFmpeg libraries
dnl Minimum version: 4.0 (ABI stable, no av_register_all needed)
dnl Recommended: 5.x or 6.x
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES(AVFORMAT, [libavformat >= 58.0],
    [AC_DEFINE(HAVE_AVFORMAT, 1, [libavformat available])],
    [AC_MSG_ERROR([libavformat >= 58.0 required])])

PKG_CHECK_MODULES(AVCODEC, [libavcodec >= 58.0],
    [AC_DEFINE(HAVE_AVCODEC, 1, [libavcodec available])],
    [AC_MSG_ERROR([libavcodec >= 58.0 required])])

PKG_CHECK_MODULES(AVUTIL, [libavutil >= 56.0],
    [AC_DEFINE(HAVE_AVUTIL, 1, [libavutil available])],
    [AC_MSG_ERROR([libavutil >= 56.0 required])])

PKG_CHECK_MODULES(SWRESAMPLE, [libswresample >= 3.0],
    [AC_DEFINE(HAVE_SWRESAMPLE, 1, [libswresample available])],
    [AC_MSG_ERROR([libswresample >= 3.0 required])])

dnl -----------------------------------------------------------------------
dnl Optional: libass for ASS/SSA subtitle rendering
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES(LIBASS, [libass >= 0.13],
    [AC_DEFINE(HAVE_LIBASS, 1, [libass subtitle rendering available])
     have_libass=yes],
    [have_libass=no
     AC_MSG_WARN([libass not found - ASS/SSA subtitle rendering disabled])])
AM_CONDITIONAL([HAVE_LIBASS], [test x$have_libass = xyes])

dnl -----------------------------------------------------------------------
dnl Enigma2 / e2 plugin headers
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES(ENIGMA2, [enigma2],
    [],
    [dnl Fallback: check for e2 headers in common locations
     AC_CHECK_HEADER([lib/service/iservice.h],
         [ENIGMA2_CFLAGS="-I."],
         [AC_CHECK_HEADER([/usr/include/enigma2/lib/service/iservice.h],
             [ENIGMA2_CFLAGS="-I/usr/include/enigma2"],
             [AC_MSG_ERROR([Enigma2 headers not found. Set ENIGMA2_CFLAGS])]
         )]
     )]
)

dnl -----------------------------------------------------------------------
dnl Python for Enigma2 plugin
dnl -----------------------------------------------------------------------
AM_PATH_PYTHON([2.7],, [AC_MSG_WARN([Python 2.7+ recommended for E2 plugin])])

dnl -----------------------------------------------------------------------
dnl Optional: ALSA for software audio output (mipsel fallback)
dnl -----------------------------------------------------------------------
PKG_CHECK_MODULES(ALSA, [alsa >= 1.0],
    [AC_DEFINE(HAVE_ALSA, 1, [ALSA audio output available])
     have_alsa=yes],
    [have_alsa=no
     AC_MSG_WARN([ALSA not found - software audio output disabled])])
AM_CONDITIONAL([HAVE_ALSA], [test x$have_alsa = xyes])

dnl -----------------------------------------------------------------------
dnl Build options
dnl -----------------------------------------------------------------------
AC_ARG_ENABLE([debug],
    AS_HELP_STRING([--enable-debug], [Enable debug output]),
    [if test x$enableval = xyes; then
        AC_DEFINE(DEBUG, 1, [Debug output enabled])
        CXXFLAGS="$CXXFLAGS -g -O0 -DDEBUG"
     fi],
    [CXXFLAGS="$CXXFLAGS -Os"])

AC_ARG_WITH([player-bin-path],
    AS_HELP_STRING([--with-player-bin-path=PATH],
                   [Path where ffmpeg-player binary is installed (default=/usr/bin)]),
    [player_bin_path=$withval],
    [player_bin_path=/usr/bin])
AC_DEFINE_UNQUOTED([SFMP_PLAYER_BIN],
    ["$player_bin_path/ffmpeg-player"],
    [Path to ffmpeg-player binary])

dnl -----------------------------------------------------------------------
dnl Compiler flags
dnl -----------------------------------------------------------------------
CXXFLAGS="$CXXFLAGS -Wall -Wno-unused-parameter -std=c++11"
CXXFLAGS="$CXXFLAGS $ENIGMA2_CFLAGS $AVFORMAT_CFLAGS $AVCODEC_CFLAGS $AVUTIL_CFLAGS"
if test x$have_libass = xyes; then
    CXXFLAGS="$CXXFLAGS $LIBASS_CFLAGS"
fi

AC_SUBST([ENIGMA2_CFLAGS])
AC_SUBST([ENIGMA2_LIBS])
AC_SUBST([AVFORMAT_CFLAGS])
AC_SUBST([AVFORMAT_LIBS])
AC_SUBST([AVCODEC_CFLAGS])
AC_SUBST([AVCODEC_LIBS])
AC_SUBST([AVUTIL_CFLAGS])
AC_SUBST([AVUTIL_LIBS])
AC_SUBST([SWRESAMPLE_CFLAGS])
AC_SUBST([SWRESAMPLE_LIBS])
AC_SUBST([LIBASS_CFLAGS])
AC_SUBST([LIBASS_LIBS])
AC_SUBST([ALSA_CFLAGS])
AC_SUBST([ALSA_LIBS])

AC_CONFIG_FILES([
    Makefile
    serviceffmpeg/Makefile
    player/Makefile
    plugin/Makefile
])
AC_OUTPUT

echo ""
echo "serviceffmpeg configuration summary:"
echo "  FFmpeg:         yes (required)"
echo "  libass:         $have_libass"
echo "  ALSA fallback:  $have_alsa"
echo "  player binary:  $player_bin_path/ffmpeg-player"
echo ""
