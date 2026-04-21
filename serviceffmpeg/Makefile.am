# serviceffmpeg/Makefile.am

# ---- Enigma2 plugin .so ------------------------------------------------
plugindir = $(libdir)/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG

plugin_LTLIBRARIES = serviceffmpeg.la

serviceffmpeg_la_SOURCES = \
	main.cpp \
	bcm_ioctls.h \
	serviceffmpeg.cpp \
	serviceffmpeg.h \
	pythonmodule.cpp

serviceffmpeg_la_CXXFLAGS = \
	$(ENIGMA2_CFLAGS) \
	$(FFMPEG_CFLAGS) \
	$(SIGCPP_CFLAGS) \
	$(PYTHON_CFLAGS) \
	-fPIC \
	-I$(top_srcdir)

serviceffmpeg_la_LIBADD = \
	$(ENIGMA2_LIBS) \
	$(SIGCPP_LIBS) \
	$(PYTHON_LIBS)

serviceffmpeg_la_LDFLAGS = \
	-module \
	-avoid-version \
	-shared \
	-no-undefined

# ---- External player binary --------------------------------------------
bin_PROGRAMS = ffmpeg-player

noinst_HEADERS = bcm_ioctls.h

ffmpeg_player_SOURCES = \
    main.cpp \
    bcm_ioctls.h        # optional — Automake erkennt Headers
                         # automatisch über noinst_HEADERS

ffmpeg_player_CXXFLAGS = \
	$(FFMPEG_CFLAGS) \
	-pthread

if HAVE_LIBASS
ffmpeg_player_CXXFLAGS += $(LIBASS_CFLAGS)
endif

if HAVE_ALSA
ffmpeg_player_CXXFLAGS += $(ALSA_CFLAGS)
endif

ffmpeg_player_LDADD = \
	$(FFMPEG_LIBS) \
	-latomic \
	-lpthread \
	-lm

if HAVE_LIBASS
ffmpeg_player_LDADD += $(LIBASS_LIBS)
endif

if HAVE_ALSA
ffmpeg_player_LDADD += $(ALSA_LIBS)
endif

ffmpeg_player_LDFLAGS = -pthread
