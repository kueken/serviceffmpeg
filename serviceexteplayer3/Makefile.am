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

serviceexteplayer3_la_LIBADD = \
	$(top_builddir)/src/exteplayer3/libexteplayer3.a

libserviceexteplayer3_la_LIBADD = \
	$(top_builddir)/src/serviceexteplayer3/libserviceexteplayer3.a

serviceexteplayer3_la_LDFLAGS = \
	-Wl,--whole-archive \
	$(top_builddir)/src/exteplayer3/libexteplayer3.a \
	-Wl,--no-whole-archive \
	-module \
	-avoid-version \
	-shared
	
libserviceexteplayer3_la_LDFLAGS = \
	-Wl,--whole-archive \
	$(top_builddir)/src/serviceexteplayer3/libserviceexteplayer3.a \
	-Wl,--no-whole-archive \
	-module -avoid-version