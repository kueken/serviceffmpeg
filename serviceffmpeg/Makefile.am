# serviceffmpeg/Makefile.am
# Enigma2 C++ service plugin shared library

# Install directory: enigma2 SystemPlugins
plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG

plugin_LTLIBRARIES = serviceffmpeg.la

serviceffmpeg_la_SOURCES = \
	serviceffmpeg.cpp \
	serviceffmpeg.h

serviceffmpeg_la_CXXFLAGS = \
	$(ENIGMA2_CFLAGS) \
	$(FFMPEG_CFLAGS) \
	-fPIC \
	-I$(top_srcdir)

serviceffmpeg_la_LIBADD = \
	$(ENIGMA2_LIBS)

serviceffmpeg_la_LDFLAGS = \
	-module \
	-avoid-version \
	-no-undefined

