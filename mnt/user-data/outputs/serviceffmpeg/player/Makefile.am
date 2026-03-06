# player/Makefile.am
# ffmpeg-player: the external player process
# Links FFmpeg directly - isolated from E2's own lib versions

bin_PROGRAMS = ffmpeg-player

ffmpeg_player_SOURCES = main.cpp

ffmpeg_player_CXXFLAGS = \
	$(FFMPEG_CFLAGS) \
	-pthread \
	-I$(top_srcdir)

if HAVE_LIBASS
ffmpeg_player_CXXFLAGS += $(LIBASS_CFLAGS)
endif

if HAVE_ALSA
ffmpeg_player_CXXFLAGS += $(ALSA_CFLAGS)
endif

ffmpeg_player_LDADD = \
	$(FFMPEG_LIBS) \
	-lpthread \
	-lm

if HAVE_LIBASS
ffmpeg_player_LDADD += $(LIBASS_LIBS)
endif

if HAVE_ALSA
ffmpeg_player_LDADD += $(ALSA_LIBS)
endif

ffmpeg_player_LDFLAGS = -pthread

# To isolate from Hisilicon vendor ffmpeg3 closed libs, uncomment:
# ffmpeg_player_LDFLAGS += -Wl,-rpath,/usr/lib/serviceffmpeg
