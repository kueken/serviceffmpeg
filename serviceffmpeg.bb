# serviceffmpeg.bb
# OpenEmbedded recipe for serviceffmpeg
# Pure FFmpeg Enigma2 media service plugin
#
# Add to your build:
#   IMAGE_INSTALL += "serviceffmpeg"
#
# This replaces servicemp3 and servicehisilicon as the media handler.
# GStreamer is no longer a runtime dependency for media playback.

SUMMARY = "Enigma2 FFmpeg media service - replaces servicemp3"
DESCRIPTION = "Pure FFmpeg-based Enigma2 media service plugin with external player process. \
    Replaces servicemp3 (GStreamer) with a lean FFmpeg-only architecture. \
    Supports all containers via libavformat, subtitle rendering via libass, \
    stream recording, HTTP/HLS/RTSP streaming."
LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=b234ee4d69f5fce4486a80fdaf4a4263"

DEPENDS = " \
    enigma2 \
    ffmpeg \
    libass \
"

RDEPENDS_${PN} = " \
    enigma2 \
    ffmpeg \
    libass \
"

# Source: local or git
SRC_URI = "git://github.com/yourfork/serviceffmpeg.git;protocol=https;branch=master"
SRCREV = "${AUTOREV}"
S = "${WORKDIR}/git"

# Alternative for local development:
# SRC_URI = "file://${TOPDIR}/../serviceffmpeg"
# S = "${WORKDIR}/serviceffmpeg"

inherit autotools pkgconfig

# Cross-compile for mipsel (et9200), ARM (Hisilicon, AML), etc.
# OE handles the toolchain via CROSS_COMPILE / CC / CXX environment

EXTRA_OECONF = " \
    --with-player-bin-path=${bindir} \
"

# On Hisilicon: optionally static-link FFmpeg in the player binary
# to isolate from vendor's ffmpeg3 closed libs
# EXTRA_OECONF += "--enable-static-player"

do_install_append() {
    # Install the .so where Enigma2 finds it
    install -d ${D}${libdir}/enigma2/python/Plugins/Extensions/serviceffmpeg
    install -m 0644 \
        ${B}/serviceffmpeg/libserviceffmpeg.so \
        ${D}${libdir}/enigma2/python/Plugins/Extensions/serviceffmpeg/

    # Install player binary
    install -d ${D}${bindir}
    install -m 0755 ${B}/player/ffmpeg-player ${D}${bindir}/
}

FILES_${PN} = " \
    ${libdir}/enigma2/python/Plugins/Extensions/serviceffmpeg/* \
    ${bindir}/ffmpeg-player \
"

# Explicitly conflict with servicemp3 and servicehisilicon
# (they both register as the 0x1001 media service type)
RCONFLICTS_${PN} = "servicemp3 servicehisilicon"
RREPLACES_${PN}  = "servicemp3 servicehisilicon"
