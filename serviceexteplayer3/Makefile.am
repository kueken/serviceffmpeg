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

# E2 Service bridge
serviceexteplayer3_la_SOURCES = \
	serviceexteplayer3.cpp \
	serviceexteplayer3.h \
	pythonmodule.cpp

serviceexteplayer3_la_CFLAGS = \
	-D_FILE_OFFSET_BITS=64 \
	-D_LARGEFILE64_SOURCE \
	-D_LARGEFILE_SOURCE \
	-DHAVE_FLV2MPEG4_CONVERTER \
	-DAUDIO_GET_PTS=VIDEO_GET_PTS \
	-Iinclude \
	-Iexternal \
	-Iexternal/flv2mpeg4

serviceexteplayer3_la_CXXFLAGS = \
	$(ENIGMA2_CFLAGS) \
	$(SIGCPP_CFLAGS) \
	$(PYTHON_CFLAGS) \
	-fPIC \
	-std=c++17 \
	-Iinclude \
	-Iexternal \
	-Iexternal/flv2mpeg4

serviceexteplayer3_la_LIBADD = \
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

