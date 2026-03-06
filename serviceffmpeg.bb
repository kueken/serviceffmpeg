# serviceffmpeg.bb
# OpenEmbedded/Bitbake recipe for scarthgap (OpenPLi 9.x)
#
# Modelled directly after OpenPLi/servicemp3's python3 branch recipe.
# Replaces servicemp3 as the enigma2 media service.
#
# Place in:
#   meta-yourlayer/recipes-enigma2/serviceffmpeg/serviceffmpeg_git.bb
#
# In your image or local.conf:
#   IMAGE_INSTALL:append = " serviceffmpeg"
#
# servicemp3 and servicehisilicon should NOT be in the image simultaneously.

SUMMARY = "Enigma2 FFmpeg media service"
DESCRIPTION = "Pure FFmpeg-based enigma2 media service plugin. \
    Replaces servicemp3 (GStreamer) with an external-process FFmpeg architecture. \
    No GStreamer runtime dependency required."
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

# ---- Source ----
# Local checkout during development:
SRC_URI = "git://github.com/yourfork/serviceffmpeg.git;protocol=https;branch=main;name=serviceffmpeg"
SRCREV = "${AUTOREV}"
PV = "1.0.0+git${SRCPV}"
S = "${WORKDIR}/git"

# ---- Build dependencies ----
DEPENDS = " \
    enigma2 \
    pkgconfig \
    ffmpeg \
    libass \
    alsa-lib \
    virtual/libc \
"

# ---- Runtime dependencies ----
RDEPENDS:${PN} = " \
    enigma2 \
    ffmpeg \
    libass \
"

# ---- Inherit ----
# autotools handles ./autogen.sh + configure + make
# pkgconfig ensures pkg-config is available for PKG_CHECK_MODULES
inherit autotools pkgconfig

# ---- Configure arguments ----
EXTRA_OECONF = " \
    --with-player-bin=${bindir}/ffmpeg-player \
"

# ---- Compiler flags ----
# scarthgap enigma2 requires C++17
CXXFLAGS:append = " -std=c++17"

# ---- Install ----
do_install:append() {
    # The autotools install puts serviceffmpeg.so into ${libdir}/enigma2/...
    # via the plugindir defined in serviceffmpeg/Makefile.am.
    # The ffmpeg-player binary goes to ${bindir} automatically.

    # Create the SystemPlugins directory if Makefile.am didn't
    install -d ${D}${libdir}/enigma2/python/Plugins/SystemPlugins/Serviceffmpeg

    # Install Python plugin stub
    install -m 0644 ${S}/plugin/plugin.py \
        ${D}${libdir}/enigma2/python/Plugins/SystemPlugins/Serviceffmpeg/plugin.py
    install -m 0644 ${S}/plugin/__init__.py \
        ${D}${libdir}/enigma2/python/Plugins/SystemPlugins/Serviceffmpeg/__init__.py

    # Move the .so to the plugin directory if libtool put it elsewhere
    if [ -f ${D}${libdir}/serviceffmpeg.so ]; then
        mv ${D}${libdir}/serviceffmpeg.so \
           ${D}${libdir}/enigma2/python/Plugins/SystemPlugins/Serviceffmpeg/serviceffmpeg.so
    fi
    # Remove libtool .la file - not needed at runtime
    find ${D} -name "*.la" -delete
}

# ---- Package files ----
FILES:${PN} = " \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/Serviceffmpeg/*.so \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/Serviceffmpeg/*.py \
    ${bindir}/ffmpeg-player \
"

# ---- Conflicts ----
# servicemp3 and servicehisilicon both register as media service 0x1001.
# Only one should be present.
RCONFLICTS:${PN} = "servicemp3 servicehisilicon"
RREPLACES:${PN}  = "servicemp3 servicehisilicon"
