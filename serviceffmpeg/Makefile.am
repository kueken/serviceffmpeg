AM_CFLAGS = \
	-Wall

AM_CPPFLAGS = \
	@PYTHON_CPPFLAGS@ \
	$(FFMPEG_CFLAGS)
	-include Python.h

AM_CXXFLAGS = \
	-Wall \
	@ENIGMA2_CFLAGS@

plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG

plugin_LTLIBRARIES = serviceffmpeg.la

servicehisilicon_la_SOURCES = \
    serviceffmpeg.cpp \
    main.cpp \
    pythonmodule.cpp

servicehisilicon_la_LDFLAGS = \
    -avoid-version \
    -module \
    -shared

serviceffmpeg_la_LIBADD = $(FFMPEG_LIBS)

