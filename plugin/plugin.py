# -*- coding: utf-8 -*-
from Plugins.Plugin import PluginDescriptor
from Screens.InfoBar import MoviePlayer
from enigma import eServiceReference


# Deine Service-ID. 4097 (0x1001) ersetzt servicemp3 und wird von Enigma2
# automatisch für lokale Medien erzeugt. Wenn du eine eigene ID willst,
# trage sie hier ein und registriere sie zusätzlich in serviceffmpeg.cpp
# (eServiceFactoryFfmpeg::id).
SERVICE_ID = 4097


def autostart(reason, **kwargs):
    """Lädt das C++-Modul beim Enigma2-Start. Der eigentliche Service-Handler
    (eServiceFactoryFfmpeg) wird in PyInit_serviceffmpeg() registriert."""
    from Plugins.SystemPlugins.ServiceFFMPEG import serviceffmpeg  # noqa: F401


def play_serviceffmpeg(session, service, **kwargs):
    """Öffnet den MoviePlayer mit unserer Service-Referenz. Erst dadurch wird
    unser Service zum 'aktuellen Service', an den InfoBarSeek die Tasten
    weiterleitet."""
    if service is None or not service.getPath():
        return
    ref = eServiceReference(SERVICE_ID, 0, service.getPath())
    session.open(MoviePlayer, service=ref)


def Plugins(**kwargs):
    return [
        PluginDescriptor(
            where=PluginDescriptor.WHERE_AUTOSTART,
            needsRestart=True,
            fnc=autostart,
        ),
        PluginDescriptor(
            name="ServiceFFMPEG",
            description="Play with FFmpeg player",
            where=PluginDescriptor.WHERE_MOVIELIST,
            needsRestart=False,
            fnc=play_serviceffmpeg,
        ),
    ]
