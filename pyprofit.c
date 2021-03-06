/**
 * libprofit integration into Python
 *
 * ICRAR - International Centre for Radio Astronomy Research
 * (c) UWA - The University of Western Australia, 2016
 * Copyright by UWA (in the framework of the ICRAR)
 * All rights reserved
 *
 * Contributed by Rodrigo Tobar
 *
 * This file is part of pyprofit.
 *
 * libprofit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libprofit is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libprofit.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <Python.h>
#include <stdbool.h>

#include "gsl/gsl_cdf.h"
#include "gsl/gsl_sf_gamma.h"

#include "profit.h"
#include "psf.h"
#include "sersic.h"
#include "sky.h"

/* Macros */
#define PYPROFIT_RAISE(str) \
	do { \
		PyErr_SetString(profit_error, str); \
		return NULL; \
	} while (0)

#define READ_DOUBLE_INTO(key, dst) \
	do { \
		PyObject *tmp = PyDict_GetItemString(item, key); \
		if( tmp != NULL ) { \
			dst = PyFloat_AsDouble(tmp); \
		} \
	} while(0)

#define READ_BOOL_INTO(key, dst) \
	do { \
		PyObject *tmp = PyDict_GetItemString(item, key); \
		if( tmp != NULL ) { \
			dst = (bool)PyObject_IsTrue(tmp); \
		} \
	} while(0)

#define READ_UNSIGNED_INT_INTO(key, dst) \
	do { \
		PyObject *tmp = PyDict_GetItemString(item, key); \
		if( tmp != NULL ) { \
			dst = (unsigned int)PyInt_AsUnsignedLongMask(tmp); \
		} \
	} while(0)

/* Exceptions */
static PyObject *profit_error;

/* Methods */
static bool *_read_boolean_matrix(PyObject *matrix, unsigned int *matrix_width, unsigned int *matrix_height) {

	bool *bools = NULL;
	Py_ssize_t width = 0, height = 0;

	if( matrix == NULL ) {
		*matrix_width = 0;
		*matrix_height = 0;
		return NULL;
	}

	height = PySequence_Size(matrix);
	for(Py_ssize_t j = 0; j!=height; j++) {
		PyObject *row = PySequence_GetItem(matrix, j);
		if( row == NULL ) {
			free(bools);
			return NULL;
		}

		/* All rows should have the same width */
		if( j == 0 ) {
			width = PySequence_Size(row);
			*matrix_height = (unsigned int)height;
			*matrix_width = (unsigned int)width;
			bools = (bool *)malloc(sizeof(bool) * *matrix_width * *matrix_height);
		}
		else {
			if( PySequence_Size(row) != width ) {
				Py_DECREF(row);
				free(bools);
				return NULL;
			}
		}

		/* Finally assign the individual values */
		for(Py_ssize_t i=0; i!=width; i++) {
			PyObject *cell = PySequence_GetItem(row, i);
			if( cell == NULL ) {
				Py_DECREF(row);
				free(bools);
				return NULL;
			}
			bools[i + j*width] = (bool)PyObject_IsTrue(cell);
			Py_DECREF(cell);
		}
		Py_DECREF(row);
	}

	return bools;
}

static void _item_to_sersic_profile(profit_profile *profile, PyObject *item) {
	profit_sersic_profile *s = (profit_sersic_profile *)profile;
	READ_DOUBLE_INTO("xcen",  s->xcen);
	READ_DOUBLE_INTO("ycen",  s->ycen);
	READ_DOUBLE_INTO("mag",   s->mag);
	READ_DOUBLE_INTO("re",    s->re);
	READ_DOUBLE_INTO("nser",  s->nser);
	READ_DOUBLE_INTO("ang",   s->ang);
	READ_DOUBLE_INTO("axrat", s->axrat);
	READ_DOUBLE_INTO("box",   s->box);

	READ_BOOL_INTO("rough",  s->rough);
	READ_DOUBLE_INTO("acc",   s->acc);
	READ_DOUBLE_INTO("re_switch", s->re_switch);
	READ_UNSIGNED_INT_INTO("resolution", s->resolution);
	READ_UNSIGNED_INT_INTO("max_recursions", s->max_recursions);

	READ_DOUBLE_INTO("re_switch", s->re_switch);
	READ_BOOL_INTO("rescale_flux", s->rescale_flux);

	READ_BOOL_INTO("adjust", s->adjust);
}

static void _item_to_sky_profile(profit_profile *profile, PyObject *item) {
	profit_sky_profile *s = (profit_sky_profile *)profile;
	READ_DOUBLE_INTO("bg", s->bg);
}

static void _item_to_psf_profile(profit_profile *profile, PyObject *item) {
	profit_psf_profile *psf = (profit_psf_profile *)profile;
	READ_DOUBLE_INTO("xcen",  psf->xcen);
	READ_DOUBLE_INTO("ycen",  psf->ycen);
	READ_DOUBLE_INTO("mag",   psf->mag);
}

void _read_profiles(profit_model *model, PyObject *profiles_dict, const char *name, void (item_to_profile)(profit_profile *, PyObject *item)) {

	PyObject *profile_sequence = PyDict_GetItemString(profiles_dict, name);
	if( profile_sequence == NULL ) {
		return;
	}

	Py_ssize_t length = PySequence_Size(profile_sequence);
	for(Py_ssize_t i = 0; i!= length; i++) {
		PyObject *item = PySequence_GetItem(profile_sequence, i);
		profit_profile *p = profit_create_profile(model, name);
		item_to_profile(p, item);
		READ_BOOL_INTO("convolve", p->convolve);
		Py_DECREF(item);
	}
}

static void _read_psf_profiles(profit_model *model, PyObject *profiles_dict) {
	_read_profiles(model, profiles_dict, "psf", &_item_to_psf_profile);
}

static void _read_sky_profiles(profit_model *model, PyObject *profiles_dict) {
	_read_profiles(model, profiles_dict, "sky", &_item_to_sky_profile);
}

static void _read_sersic_profiles(profit_model *model, PyObject *profiles_dict) {
	_read_profiles(model, profiles_dict, "sersic", &_item_to_sersic_profile);
}

static double *_read_psf(PyObject *model_dict, unsigned int *psf_width, unsigned int *psf_height) {

	double *psf = NULL;
	Py_ssize_t width = 0, height = 0;

	PyObject *matrix = PyDict_GetItemString(model_dict, "psf");
	if( matrix == NULL ) {
		*psf_width = 0;
		*psf_height = 0;
		return NULL;
	}

	height = PySequence_Size(matrix);
	for(Py_ssize_t j = 0; j!=height; j++) {
		PyObject *row = PySequence_GetItem(matrix, j);
		if( row == NULL ) {
			free(psf);
			return NULL;
		}

		/* All rows should have the same width */
		if( j == 0 ) {
			width = PySequence_Size(row);
			*psf_height = (unsigned int)height;
			*psf_width = (unsigned int)width;
			psf = (double *)malloc(sizeof(double) * *psf_width * *psf_height);
		}
		else {
			if( PySequence_Size(row) != width ) {
				Py_DECREF(row);
				free(psf);
				return NULL;
			}
		}

		/* Finally assign the individual values */
		for(Py_ssize_t i=0; i!=width; i++) {
			PyObject *cell = PySequence_GetItem(row, i);
			if( cell == NULL ) {
				Py_DECREF(row);
				free(psf);
				return NULL;
			}
			psf[i + j*width] = PyFloat_AsDouble(cell);
			Py_DECREF(cell);
		}
		Py_DECREF(row);
	}

	return psf;
}

static PyObject *pyprofit_make_model(PyObject *self, PyObject *args) {

	unsigned int i, j, psf_width = 0, psf_height = 0;
	unsigned int mask_w = 0, mask_h = 0;
	char *error;
	double *psf;
	bool *calcmask;

	PyObject *model_dict;
	if( !PyArg_ParseTuple(args, "O!", &PyDict_Type, &model_dict) ) {
		return NULL;
	}

	/* The width, height and profiles are mandatory */
	PyObject *tmp = PyDict_GetItemString(model_dict, "width");
	if( tmp == NULL ) {
		PYPROFIT_RAISE("Missing mandatory 'width' item");
	}
	long width = PyInt_AsLong(tmp);
	if( PyErr_Occurred() ) {
		return NULL;
	}
	tmp = PyDict_GetItemString(model_dict, "height");
	if( tmp == NULL ) {
		PYPROFIT_RAISE("Missing mandatory 'height' item");
	}
	long height = PyInt_AsLong(tmp);
	if( PyErr_Occurred() ) {
		return NULL;
	}
	PyObject *profiles_dict = PyDict_GetItemString(model_dict, "profiles");
	if( profiles_dict == NULL ) {
		PYPROFIT_RAISE("Missing mandatory 'profiles' item");
	}

	/* Read the psf if present */
	psf = _read_psf(model_dict, &psf_width, &psf_height);
	if( PyErr_Occurred() ) {
		return NULL;
	}
	calcmask = _read_boolean_matrix(PyDict_GetItemString(model_dict, "calcmask"), &mask_w, &mask_h);
	if( PyErr_Occurred() ) {
		return NULL;
	}
	if( calcmask && (mask_w != width || mask_h != height) ) {
		PYPROFIT_RAISE("calcmask must have same dimensions of image");
	}

	/* Create and initialize the model */
	profit_model *m = profit_create_model();
	m->width = width;
	m->height = height;
	m->res_x = width;
	m->res_y = height;
	m->psf = psf;
	m->psf_width = psf_width;
	m->psf_height = psf_height;
	m->calcmask = calcmask;
	PyObject *magzero = PyDict_GetItemString(model_dict, "magzero");
	if( magzero != NULL ) {
		m->magzero = PyFloat_AsDouble(magzero);
		if( PyErr_Occurred() ) {
			profit_cleanup(m);
			return NULL;
		}
	}

	/* Read the profiles */
	_read_sersic_profiles(m, profiles_dict);
	_read_sky_profiles(m, profiles_dict);
	_read_psf_profiles(m, profiles_dict);

	/*
	 * Go, Go, Go!
	 * This might take a few [ms], so we release the GIL
	 */
	Py_BEGIN_ALLOW_THREADS
	profit_eval_model(m);
	error = profit_get_error(m);
	Py_END_ALLOW_THREADS

	if( error ) {
		PyErr_SetString(profit_error, error);
		profit_cleanup(m);
		return NULL;
	}

	/* Copy resulting image into a 2-D tuple */
	PyObject *image_tuple = PyTuple_New(m->height);
	if( image_tuple == NULL ) {
		profit_cleanup(m);
		PYPROFIT_RAISE("Couldn't create return tuple");
	}

	for(i=0; i!=m->height; i++) {
		PyObject *row_tuple = PyTuple_New(m->width);
		if( row_tuple == NULL ) {
			profit_cleanup(m);
			PYPROFIT_RAISE("Couldn't create row tuple");
		}
		for(j=0; j!=m->width; j++) {
			PyObject *val = PyFloat_FromDouble(m->image[i*m->width + j]);
			PyTuple_SetItem(row_tuple, j, val);
		}
		PyTuple_SetItem(image_tuple, i, row_tuple);
	}

	/* Clean up and return */
	profit_cleanup(m);
	return image_tuple;
}

static PyMethodDef pyprofit_methods[] = {
    {"make_model",  pyprofit_make_model, METH_VARARGS, "Creates a profit model."},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

/* Module initialization */
PyMODINIT_FUNC
initpyprofit(void)
{
	PyObject *m = Py_InitModule("pyprofit", pyprofit_methods);
	if( m == NULL ) {
		return;
	}

	profit_error = PyErr_NewException("pyprofit.error", NULL, NULL);
	Py_INCREF(profit_error);
	PyModule_AddObject(m, "error", profit_error);
}
