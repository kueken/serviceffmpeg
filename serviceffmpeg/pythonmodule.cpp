#include "serviceffmpeg.h"

#include <Python.h>

static PyMethodDef serviceffmpegMethods[] =
{
	{NULL,NULL,0,NULL}
};

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	"serviceffmpeg",		/* m_name */
	"Module for serviceffmpeg",	/* m_doc */
	-1,				/* m_size */
	serviceffmpegMethods,	/* m_methods */
	NULL,				/* m_reload */
	NULL,				/* m_traverse */
	NULL,				/* m_clear */
	NULL,				/* m_free */
};

PyMODINIT_FUNC PyInit_serviceffmpeg(void)
{
	return PyModule_Create(&moduledef);
}
