AUTOMAKE_OPTIONS = subdir-objects

AM_CPPFLAGS = \
	-include Python.h

AM_CXXFLAGS = \
	-Wall \
	@PYTHON_CFLAGS@ \
	@ENIGMA2_CFLAGS@

plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceExteplayer3

plugin_LTLIBRARIES = serviceexteplayer3.la

serviceexteplayer3_la_SOURCES = \
	serviceexteplayer3.cpp \
	serviceexteplayer3.h \
	pythonmodule.cpp

serviceexteplayer3_la_LDFLAGS = \
	-module \
	-avoid-version \
	-shared
