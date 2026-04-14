# plugin.py  —  serviceexteplayer3

from Plugins.Plugin import PluginDescriptor

def autostart(reason, **kwargs):
        from Plugins.SystemPlugins.ServiceExteplayer3 import serviceexteplayer3

def Plugins(**kwargs):
	return [
		PluginDescriptor(where = PluginDescriptor.WHERE_AUTOSTART, needsRestart = True, fnc = autostart)
	]
