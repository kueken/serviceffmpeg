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

serviceexteplayer3_la_LIBADD = \
	$(top_builddir)/src/exteplayer3/libexteplayer3.a

serviceexteplayer3_la_LDFLAGS = \
	-module \
	-avoid-version \
	-shared
