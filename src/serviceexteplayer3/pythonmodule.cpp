#include "serviceexteplayer3.h"
#include <Python.h>

static PyMethodDef serviceexteplayer3Methods[] = {
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "serviceexteplayer3",
    "Enigma2 media service using exteplayer3",
    -1,
    serviceexteplayer3Methods,
    NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit_serviceexteplayer3(void)
{
    return PyModule_Create(&moduledef);
}
