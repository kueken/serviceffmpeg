# plugin.py  —  serviceffmpeg autostart
#
# Service ID 0x1001 (identical to servicemp3) means Enigma2's mediaplayer
# routes all local file playback directly to us without any patching needed.
# We just need to ensure the C++ .so is imported at startup.

from Plugins.Plugin import PluginDescriptor


def autostart(reason, **kwargs):
    if reason == 0:
        # Import triggers eServiceFactoryFfmpeg constructor which calls
        # eServiceCenter::addServiceFactory(0x1001, ...)
        try:
            from Plugins.SystemPlugins.ServiceFFMPEG import serviceffmpeg
        except ImportError as e:
            print("[serviceffmpeg] failed to load: %s" % e)


def Plugins(**kwargs):
    return [
        PluginDescriptor(
            name="ServiceFFMPEG",
            description="Media playback via exteplayer3 (no GStreamer)",
            where=PluginDescriptor.WHERE_AUTOSTART,
            needsRestart=True,
            fnc=autostart,
        )
    ]
