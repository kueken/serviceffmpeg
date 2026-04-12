SUMMARY     = "Enigma2 media service plugin using exteplayer3"
DESCRIPTION = "Replaces servicemp3 (GStreamer) and servicehisilicon. \
No GStreamer dependency — exteplayer3 and FFmpeg handle DVB ES injection \
directly. Universal for all boxes including mipsel."
LICENSE     = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://LICENSE;md5=b234ee4d69f5fce4486a80fdaf4a4263"

# ---- Source ----
# Local checkout during development:
SRC_URI = "git://github.com/yourfork/serviceffmpeg.git;protocol=https;branch=exteplayer3;name=serviceexteplayer"
SRCREV = "${AUTOREV}"
PV = "2.0.0+git${SRCPV}"
S = "${WORKDIR}/git"

# Build-time: enigma2 headers + ffmpeg + python3
DEPENDS = "enigma2 ffmpeg python3 python3-native"

# Runtime: only ffmpeg libs needed — exteplayer3 is built-in
RDEPENDS:${PN} = "enigma2 ffmpeg"

inherit autotools pkgconfig pythonnative python3-compileall

PACKAGE_ARCH = "${MACHINE_ARCH}"

PROVIDES  += "virtual/enigma2-mediaservice"
RPROVIDES:${PN} += "virtual/enigma2-mediaservice"

EXTRA_OECONF = " \
    BUILD_SYS=${BUILD_SYS} \
    HOST_SYS=${HOST_SYS} \
    STAGING_INCDIR=${STAGING_INCDIR} \
    STAGING_LIBDIR=${STAGING_LIBDIR} \
"

FILES:${PN} = " \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/ServiceExteplayer3/*.so \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/ServiceExteplayer3/*.pyc \
    ${libdir}/enigma2/python/Plugins/SystemPlugins/ServiceExteplayer3/__init__.pyc \
"

# Cannot coexist with other media services
RCONFLICTS:${PN} = "enigma2-plugin-systemplugins-servicemp3 \
                    enigma2-plugin-systemplugins-servicehisilicon \
                    enigma2-plugin-systemplugins-serviceapp"
RREPLACES:${PN}  = "enigma2-plugin-systemplugins-servicemp3 \
                    enigma2-plugin-systemplugins-servicehisilicon"
