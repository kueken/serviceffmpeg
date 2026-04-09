# plugin.py  —  serviceffmpeg
#
# Registers serviceffmpeg (.so) at autostart and provides a
# MoviePlayer-based player that correctly handles evStart/ServiceEventTracker.

from Plugins.Plugin import PluginDescriptor
from Screens.InfoBar import InfoBar, MoviePlayer
from enigma import eServiceReference


class ServiceFfmpegPlayer(MoviePlayer):
    def __init__(self, session, service):
        MoviePlayer.__init__(self, session, service)
        self.skinName = ["ServiceFfmpegPlayer", "MoviePlayer"]
        self.servicelist = InfoBar.instance and InfoBar.instance.servicelist

    def handleLeave(self, how):
        if how == "ask":
            from Screens.MessageBox import MessageBox
            self.session.openWithCallback(
                self.leavePlayerConfirmed,
                MessageBox, _("Stop playing this movie?"))
        else:
            self.close()

    def leavePlayerConfirmed(self, answer):
        if answer:
            self.close()


def autostart(reason, **kwargs):
    if reason == 0:
        try:
            from Plugins.SystemPlugins.ServiceFFMPEG import serviceffmpeg
        except ImportError as e:
            print("[serviceffmpeg] failed to load: %s" % e)


def filescan_open(list, session, **kwargs):
    """Open files directly with ServiceFfmpegPlayer (MoviePlayer based)"""
    if not list:
        return
    # Use first file — MoviePlayer handles single file playback
    f = list[0]
    path = f.path
    ref = eServiceReference(0x1337, 0, path)
    session.open(ServiceFfmpegPlayer, service=ref)


def filescan(**kwargs):
    from Components.Scanner import Scanner, ScanPath
    return [
        Scanner(
            mimetypes=[
                "video/mpeg", "video/mp2t", "video/x-msvideo", "video/mkv",
                "video/x-ms-wmv", "video/x-matroska", "video/mp4", "video/avi",
                "video/divx", "video/x-flv", "video/quicktime", "video/x-ms-asf",
                "video/3gpp", "video/3gpp2", "video/mts",
            ],
            paths_to_scan=[
                ScanPath(path="", with_subdirs=False),
            ],
            name="ServiceFFMPEG",
            description=_("Play with serviceffmpeg (exteplayer3)"),
            openfnc=filescan_open,
        ),
    ]


def Plugins(**kwargs):
    return [
        PluginDescriptor(
            name="ServiceFFMPEG",
            description="Media playback via exteplayer3 (no GStreamer)",
            where=PluginDescriptor.WHERE_AUTOSTART,
            needsRestart=True,
            fnc=autostart,
        ),
        PluginDescriptor(
            name=_("ServiceFFMPEG"),
            description=_("Play with serviceffmpeg (exteplayer3)"),
            where=PluginDescriptor.WHERE_FILESCAN,
            needsRestart=False,
            fnc=filescan,
        ),
    ]
