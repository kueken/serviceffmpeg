# plugin.py  —  serviceexteplayer3

from Plugins.Plugin import PluginDescriptor

def autostart(reason, **kwargs):
    try:
        from Plugins.SystemPlugins.ServiceExteplayer3 import serviceexteplayer3
        serviceexteplayer3.initialize()   # falls vorhanden
    except Exception as e:
        print("[ServiceExteplayer3] Autostart failed:", e)
