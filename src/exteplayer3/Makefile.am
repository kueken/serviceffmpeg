AUTOMAKE_OPTIONS = subdir-objects
CC ?= gcc
CFLAGS += -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE -DHAVE_FLV2MPEG4_CONVERTER
CFLAGS += -DAUDIO_GET_PTS=VIDEO_GET_PTS
LDFLAGS += -lswscale -ldl -lpthread -lavformat -lavcodec -lavutil -lswresample

SRC_DIR := src
OBJ_DIR := obj
BIN := exteplayer3

AM_CPPFLAGS   = -I$(srcdir)/include
AM_CPPFLAGS  += -I$(top_srcdir)/include
AM_CPPFLAGS  += -I$(srcdir)/external

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

serviceexteplayer3_a_SOURCES = $(SOURCE_FILES)

OBJS := $(patsubst %.c, $(OBJ_DIR)/%.o, $(SOURCE_FILES))

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $(BIN)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Iexternal -Iexternal/flv2mpeg4 -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN)
