```cpp
/*
 * pythonmodule.cpp
 *
 * Python-Modul-Einstieg für serviceffmpeg.so
 *
 * Die Service-Factory wird explizit in PyInit_serviceffmpeg()
 * erzeugt und über s_factory während der Laufzeit gehalten.
 */

#include "serviceffmpeg.h"
#include <Python.h>
#include <new>

/* Hält die Factory am Leben, solange das Modul geladen ist. */
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
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_serviceffmpeg(void)
{
    /* Zuerst das Python-Modul erzeugen. */
    PyObject *module = PyModule_Create(&moduledef);
    if (!module)
        return NULL;

    /*
     * Factory explizit erzeugen.
     * s_factory hält die Referenz über ePtr.
     */
    if (!s_factory)
    {
        eServiceFactoryFfmpeg *factory =
            new (std::nothrow) eServiceFactoryFfmpeg();

        if (!factory)
        {
            Py_DECREF(module);
            PyErr_NoMemory();
            return NULL;
        }

        s_factory = factory;
    }

    return module;
}

