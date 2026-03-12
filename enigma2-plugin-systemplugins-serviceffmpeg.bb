SUMMARY     = "Enigma2 media service plugin using exteplayer3"
DESCRIPTION = "Replaces servicemp3 (GStreamer) and servicehisilicon. \
No GStreamer dependency — exteplayer3 handles DVB ES injection."
LICENSE     = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

# ---- Source ----
# Local checkout during development:
SRC_URI = "git://github.com/yourfork/serviceffmpeg.git;protocol=https;branch=main;name=serviceffmpeg"
SRCREV = "${AUTOREV}"
PV = "2.0.0+git${SRCPV}"
S = "${WORKDIR}/git"

# Build-time: enigma2 headers + python3 only (no ffmpeg needed here)
DEPENDS = "enigma2 pkgconfig python3 python3-native"

# Runtime: exteplayer3 does the actual playback (PN matches exteplayer3_git.bb → "exteplayer3")
RDEPENDS:${PN} = "enigma2 exteplayer3"

inherit autotools pkgconfig pythonnative

PACKAGE_ARCH = "${MACHINE_ARCH}"

PROVIDES  += "virtual-enigma2-mediaservice"
RPROVIDES:${PN} += "virtual-enigma2-mediaservice"

EXTRA_OECONF = " \
    STAGING_INCDIR=${STAGING_INCDIR} \
    STAGING_LIBDIR=${STAGING_LIBDIR} \
"

do_install:append() {
    # Remove libtool artefacts and Python cache
    find ${D} -name "*.la" -delete
    find ${D} -name "*.a"  -delete
    find ${D} -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true
}

FILES:${PN} = " \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG/*.so \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/ServiceFFMPEG/*.py \
"

# Cannot coexist with other media services
RCONFLICTS:${PN} = "enigma2-plugin-systemplugins-servicemp3 \
                    enigma2-plugin-systemplugins-servicehisilicon"
RREPLACES:${PN}  = "enigma2-plugin-systemplugins-servicemp3 \
                    enigma2-plugin-systemplugins-servicehisilicon"
