AUTOMAKE_OPTIONS = subdir-objects

AM_CFLAGS = \
	-Wall \
	-D_LARGEFILE_SOURCE \
	-D_FILE_OFFSET_BITS=64 \
	-D_LARGEFILE64_SOURCE \
	-DHAVE_FLV2MPEG4_CONVERTER \
	-DAUDIO_GET_PTS=VIDEO_GET_PTS
	
CXXFLAGS = -Wall

# Static library — linked into serviceexteplayer3.so
noinst_LIBRARIES = libexteplayer3.a

AM_CPPFLAGS   = -I$(srcdir)/include
AM_CPPFLAGS  += -I$(top_srcdir)/include
AM_CPPFLAGS  += -I$(srcdir)/external
AM_CPPFLAGS  += -I$(srcdir)/external/flv2mpeg4

AM_CXXFLAGS   = -fno-rtti -fno-exceptions -fno-strict-aliasing

SOURCE_FILES  = container/container.c
SOURCE_FILES += container/container_ffmpeg.c
SOURCE_FILES += manager/manager.c
SOURCE_FILES += manager/audio.c
SOURCE_FILES += manager/video.c
SOURCE_FILES += manager/subtitle.c
SOURCE_FILES += output/graphic_subtitle.c
SOURCE_FILES += output/output_subtitle.c
SOURCE_FILES += output/output.c
SOURCE_FILES += output/writer/common/pes.c
SOURCE_FILES += output/writer/common/misc.c
SOURCE_FILES += output/writer/common/writer.c
SOURCE_FILES += output/linuxdvb_buffering.c
SOURCE_FILES += playback/playback.c
SOURCE_FILES += external/ffmpeg/src/bitstream.c
SOURCE_FILES += external/ffmpeg/src/latmenc.c
SOURCE_FILES += external/ffmpeg/src/mpeg4audio.c
SOURCE_FILES += external/ffmpeg/src/xiph.c
SOURCE_FILES += external/flv2mpeg4/src/m4vencode.c
SOURCE_FILES += external/flv2mpeg4/src/flvdecoder.c
SOURCE_FILES += external/flv2mpeg4/src/dcprediction.c
SOURCE_FILES += external/flv2mpeg4/src/flv2mpeg4.c
SOURCE_FILES += external/plugins/src/png.c

SOURCE_FILES += \
	output/linuxdvb_mipsel.c \
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
	output/writer/mipsel/vc1.c

# exteplayer3 binary — installed to /usr/bin/exteplayer3
bin_PROGRAMS = exteplayer3

exteplayer3_SOURCES = \
	main/exteplayer.c \
	$(SOURCE_FILES)

libexteplayer3_a_SOURCES = $(SOURCE_FILES)

LDFLAGS += -lswscale -ldl -lpthread -lavformat -lavcodec -lavutil -lswresample
