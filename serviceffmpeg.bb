SUMMARY = "Enigma2 FFmpeg media service"
DESCRIPTION = "Pure FFmpeg enigma2 media service. Replaces servicemp3."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

# ---- Source ----
# Local checkout during development:
SRC_URI = "git://github.com/yourfork/serviceffmpeg.git;protocol=https;branch=main;name=serviceffmpeg"
SRCREV = "${AUTOREV}"
PV = "1.0.0+git${SRCPV}"
S = "${WORKDIR}/git"

DEPENDS = " \
    enigma2 \
    pkgconfig \
    ffmpeg \
    libass \
    alsa-lib \
    python3 \
    python3-native \
"

RDEPENDS:${PN} = "enigma2 ffmpeg libass"

inherit autotools pkgconfig pythonnative

# Pass STAGING_INCDIR and STAGING_LIBDIR so configure finds Python headers
# This is the same approach enigma2.bb uses
EXTRA_OECONF = " \
    STAGING_INCDIR=${STAGING_INCDIR} \
    STAGING_LIBDIR=${STAGING_LIBDIR} \
"

do_install:append() {
    find ${D} -name "*.la" -delete
}

FILES:${PN} = " \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG/*.so \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG/*.py \
    ${bindir}/ffmpeg-player \
"

RCONFLICTS:${PN} = "servicemp3 servicehisilicon"
RREPLACES:${PN}  = "servicemp3 servicehisilicon"
