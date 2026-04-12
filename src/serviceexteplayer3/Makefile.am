plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceExteplayer3

AUTOMAKE_OPTIONS = subdir-objects

plugin_LTLIBRARIES = serviceexteplayer3.la

# E2 Service bridge
serviceexteplayer3_la_SOURCES = \
	serviceexteplayer3.cpp \
	serviceexteplayer3.h \
	pythonmodule.cpp

# exteplayer3 C sources — built directly into the .so
serviceexteplayer3_la_SOURCES += \
	main/exteplayer.c \
	container/container.c \
	container/container_ffmpeg.c \
	container/buff_ffmpeg.c \
	container/wrapped_ffmpeg.c \
	container/flv2mpeg4_ffmpeg.c \
	container/mpeg4p2_ffmpeg.c \
	manager/manager.c \
	manager/audio.c \
	manager/video.c \
	manager/subtitle.c \
	output/output.c \
	output/output_subtitle.c \
	output/linuxdvb_buffering.c \
	output/linuxdvb_mipsel.c \
	output/linuxdvb_fake.c \
	output/graphic_subtitle.c \
	output/writer/common/pes.c \
	output/writer/common/misc.c \
	output/writer/common/writer.c \
	output/writer/mipsel/writer.c \
	output/writer/mipsel/aac.c \
	output/writer/mipsel/ac3.c \
	output/writer/mipsel/bcma.c \
	output/writer/mipsel/mp3.c \
	output/writer/mipsel/pcm.c \
	output/writer/mipsel/lpcm.c \
	output/writer/mipsel/dts.c \
	output/writer/mipsel/amr.c \
	output/writer/mipsel/h265.c \
	output/writer/mipsel/h264.c \
	output/writer/mipsel/mjpeg.c \
	output/writer/mipsel/mpeg2.c \
	output/writer/mipsel/mpeg4.c \
	output/writer/mipsel/divx3.c \
	output/writer/mipsel/vp.c \
	output/writer/mipsel/wmv.c \
	output/writer/mipsel/vc1.c \
	playback/playback.c \
	tools/debug.c \
	tools/strbuffer.c \
	external/ffmpeg/src/bitstream.c \
	external/ffmpeg/src/latmenc.c \
	external/ffmpeg/src/mpeg4audio.c \
	external/ffmpeg/src/xiph.c \
	external/flv2mpeg4/src/m4vencode.c \
	external/flv2mpeg4/src/flvdecoder.c \
	external/flv2mpeg4/src/dcprediction.c \
	external/flv2mpeg4/src/flv2mpeg4.c \
	external/plugins/src/png.c

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
	
AM_CPPFLAGS = \
    -I$(top_srcdir)/src/serviceexteplayer3 \
    -I$(top_srcdir)/src/serviceexteplayer3/external \
    -I$(top_srcdir)/src/serviceexteplayer3/include
