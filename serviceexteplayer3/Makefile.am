AUTOMAKE_OPTIONS = subdir-objects

AM_CFLAGS = \
	-Wall

AM_CPPFLAGS = \
	@PYTHON_CPPFLAGS@ \
	-include Python.h

AM_CXXFLAGS = \
	-Wall \
	@ENIGMA2_CFLAGS@

plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceExteplayer3

plugin_LTLIBRARIES = serviceexteplayer3.la

serviceexteplayer3_la_SOURCES = \
	serviceexteplayer3.cpp \
	serviceexteplayer3.h \
	pythonmodule.cpp

serviceexteplayer3_la_CXXFLAGS = \
	$(ENIGMA2_CFLAGS) \
	$(SIGCPP_CFLAGS) \
	$(PYTHON_CFLAGS) \
	-fPIC \
	-std=c++17 \
	-I$(top_srcdir)/src/exteplayer3/include \
	-I$(top_srcdir)/src/exteplayer3/external \
	-I$(top_srcdir)/src/exteplayer3/external/flv2mpeg4

serviceexteplayer3_la_LIBADD = \
	$(top_builddir)/src/exteplayer3/libexteplayer3.a \
	$(ENIGMA2_LIBS) \
	$(SIGCPP_LIBS) \
	$(PYTHON_LIBS) \
	$(FFMPEG_LIBS) \
	-ldl \
	-lpthread \
	-lpng

serviceexteplayer3_la_LDFLAGS = \
	-module \
	-avoid-version \
	-shared
