# plugin.py  —  serviceffmpeg autostart + MoviePlayer integration
#
# Service ID 0x1001 — transparent replacement for servicemp3.
# Uses MoviePlayer as base class (same as serviceapp) to ensure
# correct ServiceEventTracker stack and evStart delivery.

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


def play_serviceffmpeg(session, service, **kwargs):
    ref = eServiceReference(0x1001, 0, service.getPath())
    session.open(ServiceFfmpegPlayer, service=ref)


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
            name="ServiceFFMPEG",
            description="Play with serviceffmpeg (exteplayer3)",
            where=PluginDescriptor.WHERE_MOVIELIST,
            needsRestart=False,
            fnc=play_serviceffmpeg,
        ),
    ]
