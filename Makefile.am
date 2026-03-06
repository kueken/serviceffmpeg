# serviceffmpeg/Makefile.am
# Enigma2 C++ service plugin - shared library loaded at E2 startup

pkglib_LTLIBRARIES = serviceffmpeg.la

serviceffmpeg_la_SOURCES = \
	serviceffmpeg.cpp \
	serviceffmpeg.h

serviceffmpeg_la_CXXFLAGS = \
	$(ENIGMA2_CFLAGS) \
	$(FFMPEG_CFLAGS) \
	-fPIC \
	-I$(top_srcdir)

# The .so itself needs no FFmpeg at link time (E2 sysroot already has them).
# Only enigma2 and standard libs needed.
serviceffmpeg_la_LIBADD = \
	$(ENIGMA2_LIBS)

serviceffmpeg_la_LDFLAGS = \
	-module \
	-avoid-version \
	-no-undefined

# Install into enigma2 SystemPlugins (same location as servicemp3)
plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/Serviceffmpeg
plugin_LTLIBRARIES = serviceffmpeg.la
