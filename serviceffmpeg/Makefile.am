# serviceffmpeg/Makefile.am
# No own player binary — exteplayer3 (RDEPENDS) handles all playback.

AM_CFLAGS = \
	-Wall

AM_CPPFLAGS = \
	-include Python.h

AM_CXXFLAGS = \
	-Wall \
	@ENIGMA2_CFLAGS@

plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG

plugin_LTLIBRARIES = serviceffmpeg.la

serviceffmpeg_la_SOURCES = \
	serviceffmpeg.cpp \
	pythonmodule.cpp

serviceffmpeg_la_CXXFLAGS = \
	$(ENIGMA2_CFLAGS) \
	$(SIGCPP_CFLAGS) \
	$(PYTHON_CFLAGS) \
	-fPIC \
	-I$(top_srcdir)

serviceffmpeg_la_LIBADD = \
	$(ENIGMA2_LIBS) \
	$(SIGCPP_LIBS) \
	$(PYTHON_LIBS)

serviceffmpeg_la_LDFLAGS = \
	-module \
	-avoid-version \
	-shared
