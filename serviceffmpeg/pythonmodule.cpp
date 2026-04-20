/*
 * pythonmodule.cpp
 *
 * Python-Modul-Einstieg für serviceffmpeg.so
 *
 * ROOT CAUSE (aus Debuglog analysiert):
 *   eAutoInitPtr<eServiceFactoryFfmpeg> in serviceffmpeg.cpp wird
 *   ausgeführt wenn die .so geladen wird. Zu diesem Zeitpunkt
 *   (Python Autostart, nach E2-Mainloop-Start) ist eServiceCenter
 *   verfügbar — der Konstruktor SOLLTE funktionieren.
 *
 *   ABER: Im Log fehlt "[serviceffmpeg] registered, replacing servicemp3".
 *   Das bedeutet: der eAutoInitPtr-Konstruktor läuft NICHT oder
 *   eServiceCenter::getPrivInstance() liefert NULL.
 *
 *   Grund: eAutoInitPtr verwendet einen eigenen Init-Mechanismus
 *   der nur beim Programmstart (vor dem Mainloop) aktiv ist.
 *   Wird die .so später geladen, constructed er das Objekt NICHT
 *   automatisch — erst beim ersten Zugriff (lazy init).
 *
 * FIX:
 *   In PyInit_serviceffmpeg() eine statische Factory-Instanz anlegen.
 *   Diese hält die Referenz für die gesamte Laufzeit und registriert
 *   sich im Konstruktor in eServiceCenter.
 */

#include "serviceffmpeg.h"
#include <Python.h>

/* Statische Instanz — hält die Factory am Leben für die Laufzeit der .so */
static ePtr<eServiceFactoryFfmpeg> s_factory;

static PyMethodDef serviceffmpegMethods[] =
{
    { NULL, NULL, 0, NULL }
};

static struct PyModuleDef moduledef =
{
    PyModuleDef_HEAD_INIT,
    "serviceffmpeg",
    "Module for serviceffmpeg",
    -1,
    serviceffmpegMethods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_serviceffmpeg(void)
{
    /*
     * Factory explizit instanziieren.
     * Der Konstruktor von eServiceFactoryFfmpeg ruft addServiceFactory()
     * auf — zu diesem Zeitpunkt läuft der E2-Mainloop und
     * eServiceCenter ist garantiert verfügbar.
     *
     * s_factory hält die Referenz (via ePtr<>) für die gesamte
     * Laufzeit der .so — verhindert vorzeitige Destruktion.
     */
    if (!s_factory)
    {
        s_factory = new eServiceFactoryFfmpeg();
        /* Konstruktor hat bereits eDebug ausgegeben wenn erfolgreich */
    }

    return PyModule_Create(&moduledef);
}

/*
 * HINWEIS für serviceffmpeg.cpp:
 * ================================
 * Die letzten zwei Zeilen in serviceffmpeg.cpp:
 *
 *   eAutoInitPtr<eServiceFactoryFfmpeg> init_eServiceFactoryFfmpeg(
 *       eAutoInitNumbers::service + 1, "eServiceFactoryFfmpeg");
 *
 * ENTFERNEN oder ersetzen durch:
 *
 *   // Factory wird durch PyInit_serviceffmpeg() in pythonmodule.cpp registriert
 *
 * Grund: eAutoInitPtr konstruiert das Objekt entweder zu früh (kein eServiceCenter)
 * oder zu spät (nach dem eInit-Lauf ohne Log-Eintrag). In beiden Fällen ist das
 * Ergebnis unzuverlässig für dynamisch geladene .so Plugins.
 * Die explizite Registrierung in PyInit_serviceffmpeg() ist die korrekte Methode
 * für Python-Plugin-gebundene C++ Service-Factories in Enigma2.
 */
