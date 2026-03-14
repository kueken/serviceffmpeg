# serviceffmpeg/Makefile.am
# No own player binary — exteplayer3 (RDEPENDS) handles all playback.

plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG

plugin_LTLIBRARIES = serviceffmpeg.la

serviceffmpeg_la_SOURCES = \
	serviceffmpeg.cpp \
	serviceffmpeg.h \
	pythonmodule.cpp

serviceffmpeg_la_CXXFLAGS = \
	$(ENIGMA2_CFLAGS) \
	$(PYTHON_CFLAGS) \
	-fPIC \
	-I$(top_srcdir)

serviceffmpeg_la_LIBADD = \
	$(ENIGMA2_LIBS) \
	$(PYTHON_LIBS) \
	$(shell pkg-config --libs sigc++-3.0)

serviceffmpeg_la_LDFLAGS = \
	-module \
	-avoid-version \
	-no-undefined
