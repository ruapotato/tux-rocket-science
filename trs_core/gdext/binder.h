/*
 * Tux Rocket Science
 * Copyright (C) 2026 Tux Rocket Science contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* Minimal pure-C binding layer over gdextension_interface.h.
 * Methods are described by a table of trs_method_def; the binder generates
 * both the Variant call and the ptrcall paths and does all marshalling. */
#ifndef TRS_GDEXT_BINDER_H
#define TRS_GDEXT_BINDER_H

#include "gdextension_interface.h"

#include <stdint.h>

typedef enum {
	TRS_T_VOID = 0,
	TRS_T_BOOL,
	TRS_T_INT,
	TRS_T_FLOAT,
	TRS_T_STRING, /* UTF-8, owned by the binder */
	TRS_T_F64ARR, /* PackedFloat64Array */
} trs_type;

typedef struct {
	trs_type type;
	union {
		int b;
		int64_t i;
		double f;
		struct {
			char *s; /* NUL-terminated */
			int64_t slen;
		};
		struct {
			double *data;
			int64_t n;
		} arr;
	};
} trs_val;

#define TRS_MAX_ARGS 8

/* Return values: set ret->type is preset by the binder; fill the field.
 * Strings/arrays returned must be malloc()ed; the binder frees them. */
typedef void (*trs_method_fn)(void *self, const trs_val *args, trs_val *ret);

typedef struct {
	const char *name;
	trs_method_fn fn;
	trs_type ret;
	int nargs;
	trs_type args[TRS_MAX_ARGS];
	const char *arg_names[TRS_MAX_ARGS];
} trs_method_def;

typedef struct {
	const char *name;
	const char *parent;
	void *(*create)(void);
	void (*destroy)(void *self);
	const trs_method_def *methods;
	int nmethods;
} trs_class_def;

int trs_binder_init(GDExtensionInterfaceGetProcAddress gpa, GDExtensionClassLibraryPtr lib);
void trs_binder_register(const trs_class_def *cls);
void trs_print_error(const char *msg, const char *func, const char *file, int line);

char *trs_strdup_printf(const char *fmt, ...);

#endif
