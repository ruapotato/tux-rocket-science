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

#include "binder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Opaque storage big enough for any builtin we marshal (Variant is 24 bytes,
 * PackedFloat64Array 16, String/StringName 8 in float_64 and float_32). */
typedef struct {
	_Alignas(8) uint8_t b[32];
} opaque;

static GDExtensionClassLibraryPtr g_lib;

#define API(type, name) static GDExtensionInterface##type p_##name
API(PrintError, print_error);
API(StringNameNewWithUtf8Chars, string_name_new_with_utf8_chars);
API(StringNewWithUtf8CharsAndLen2, string_new_with_utf8_chars_and_len2);
API(StringToUtf8Chars, string_to_utf8_chars);
API(GetVariantFromTypeConstructor, get_variant_from_type_constructor);
API(GetVariantToTypeConstructor, get_variant_to_type_constructor);
API(VariantGetType, variant_get_type);
API(VariantDestroy, variant_destroy);
API(VariantGetPtrConstructor, variant_get_ptr_constructor);
API(VariantGetPtrDestructor, variant_get_ptr_destructor);
API(VariantGetPtrBuiltinMethod, variant_get_ptr_builtin_method);
API(PackedFloat64ArrayOperatorIndex, packed_float64_array_operator_index);
API(PackedFloat64ArrayOperatorIndexConst, packed_float64_array_operator_index_const);
API(ClassdbConstructObject3, classdb_construct_object3);
API(ClassdbGetMethodBind, classdb_get_method_bind);
API(ObjectMethodBindPtrcall, object_method_bind_ptrcall);
API(ObjectSetInstance, object_set_instance);
API(ClassdbRegisterExtensionClass6, classdb_register_extension_class6);
API(ClassdbRegisterExtensionClassMethod, classdb_register_extension_class_method);
#undef API

static GDExtensionVariantFromTypeConstructorFunc to_variant[GDEXTENSION_VARIANT_TYPE_VARIANT_MAX];
static GDExtensionTypeFromVariantConstructorFunc from_variant[GDEXTENSION_VARIANT_TYPE_VARIANT_MAX];
static GDExtensionPtrDestructor destroy_string, destroy_f64arr;
static GDExtensionPtrConstructor construct_f64arr;
static GDExtensionPtrBuiltInMethod f64arr_size, f64arr_resize;
static GDExtensionMethodBindPtr mb_notification;

static void sn_new(opaque *o, const char *s) {
	memset(o, 0, sizeof(*o));
	p_string_name_new_with_utf8_chars(o, s);
}

static void str_new(opaque *o, const char *s, int64_t len) {
	memset(o, 0, sizeof(*o));
	p_string_new_with_utf8_chars_and_len2(o, s, len);
}

void trs_print_error(const char *msg, const char *func, const char *file, int line) {
	if (p_print_error)
		p_print_error(msg, func, file, line, 0);
	else
		fprintf(stderr, "ERROR: %s (%s %s:%d)\n", msg, func, file, line);
}

char *trs_strdup_printf(const char *fmt, ...) {
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	char *s = malloc((size_t)(n < 0 ? 0 : n) + 1);
	if (s)
		vsnprintf(s, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return s;
}

#define LOAD(name)                                                       \
	do {                                                                 \
		p_##name = (void *)gpa(#name);                                   \
		if (!p_##name) {                                                 \
			fprintf(stderr, "trs_core: missing interface %s\n", #name); \
			return 0;                                                    \
		}                                                                \
	} while (0)

int trs_binder_init(GDExtensionInterfaceGetProcAddress gpa, GDExtensionClassLibraryPtr lib) {
	g_lib = lib;
	LOAD(print_error);
	LOAD(string_name_new_with_utf8_chars);
	LOAD(string_new_with_utf8_chars_and_len2);
	LOAD(string_to_utf8_chars);
	LOAD(get_variant_from_type_constructor);
	LOAD(get_variant_to_type_constructor);
	LOAD(variant_get_type);
	LOAD(variant_destroy);
	LOAD(variant_get_ptr_constructor);
	LOAD(variant_get_ptr_destructor);
	LOAD(variant_get_ptr_builtin_method);
	LOAD(packed_float64_array_operator_index);
	LOAD(packed_float64_array_operator_index_const);
	LOAD(classdb_construct_object3);
	LOAD(classdb_get_method_bind);
	LOAD(object_method_bind_ptrcall);
	LOAD(object_set_instance);
	LOAD(classdb_register_extension_class6);
	LOAD(classdb_register_extension_class_method);
	return 1;
}

/* Called at SCENE init level, when builtin types are available. */
static void binder_late_init(void) {
	static int done;
	if (done)
		return;
	done = 1;
	const GDExtensionVariantType ts[] = {
		GDEXTENSION_VARIANT_TYPE_BOOL, GDEXTENSION_VARIANT_TYPE_INT, GDEXTENSION_VARIANT_TYPE_FLOAT,
		GDEXTENSION_VARIANT_TYPE_STRING, GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY
	};
	for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
		to_variant[ts[i]] = p_get_variant_from_type_constructor(ts[i]);
		from_variant[ts[i]] = p_get_variant_to_type_constructor(ts[i]);
	}
	destroy_string = p_variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_STRING);
	destroy_f64arr = p_variant_get_ptr_destructor(GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY);
	construct_f64arr = p_variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY, 0);
	opaque sn;
	sn_new(&sn, "size");
	f64arr_size = p_variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY, &sn, 3173160232LL);
	sn_new(&sn, "resize");
	f64arr_resize = p_variant_get_ptr_builtin_method(GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY, &sn, 848867239LL);
	opaque cls, m;
	sn_new(&cls, "Object");
	sn_new(&m, "notification");
	mb_notification = p_classdb_get_method_bind(&cls, &m, 4023243586LL);
}

static GDExtensionVariantType gd_type(trs_type t) {
	switch (t) {
	case TRS_T_VOID: return GDEXTENSION_VARIANT_TYPE_NIL;
	case TRS_T_BOOL: return GDEXTENSION_VARIANT_TYPE_BOOL;
	case TRS_T_INT: return GDEXTENSION_VARIANT_TYPE_INT;
	case TRS_T_FLOAT: return GDEXTENSION_VARIANT_TYPE_FLOAT;
	case TRS_T_STRING: return GDEXTENSION_VARIANT_TYPE_STRING;
	case TRS_T_F64ARR: return GDEXTENSION_VARIANT_TYPE_PACKED_FLOAT64_ARRAY;
	}
	return GDEXTENSION_VARIANT_TYPE_NIL;
}

/* ---- builtin <-> C conversions ------------------------------------------ */

static void read_string(const void *gd_string, trs_val *v) {
	int64_t n = p_string_to_utf8_chars(gd_string, NULL, 0);
	v->s = malloc((size_t)n + 1);
	p_string_to_utf8_chars(gd_string, v->s, n);
	v->s[n] = 0;
	v->slen = n;
}

static void read_f64arr(const void *arr, trs_val *v) {
	int64_t n = 0;
	f64arr_size((void *)arr, NULL, &n, 0);
	v->arr.n = n;
	v->arr.data = malloc(sizeof(double) * (size_t)(n ? n : 1));
	for (int64_t i = 0; i < n; i++)
		v->arr.data[i] = *p_packed_float64_array_operator_index_const(arr, i);
}

/* Writes into an *uninitialized* PackedFloat64Array. */
static void write_f64arr(void *arr, const trs_val *v) {
	construct_f64arr(arr, NULL);
	int64_t n = v->arr.n, ok = 0;
	const void *args[1] = { &n };
	f64arr_resize(arr, args, &ok, 1);
	for (int64_t i = 0; i < n; i++)
		*p_packed_float64_array_operator_index(arr, i) = v->arr.data[i];
}

static void free_val(trs_val *v) {
	if (v->type == TRS_T_STRING)
		free(v->s);
	else if (v->type == TRS_T_F64ARR)
		free(v->arr.data);
}

/* ---- per-method trampolines -------------------------------------------- */

static void method_ptrcall(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstTypePtr *p_args, GDExtensionTypePtr r_ret) {
	const trs_method_def *m = ud;
	trs_val args[TRS_MAX_ARGS], ret;
	memset(args, 0, sizeof(args));
	memset(&ret, 0, sizeof(ret));
	for (int i = 0; i < m->nargs; i++) {
		args[i].type = m->args[i];
		switch (m->args[i]) {
		case TRS_T_BOOL: args[i].b = *(const uint8_t *)p_args[i] != 0; break;
		case TRS_T_INT: args[i].i = *(const int64_t *)p_args[i]; break;
		case TRS_T_FLOAT: args[i].f = *(const double *)p_args[i]; break;
		case TRS_T_STRING: read_string(p_args[i], &args[i]); break;
		case TRS_T_F64ARR: read_f64arr(p_args[i], &args[i]); break;
		case TRS_T_VOID: break;
		}
	}
	ret.type = m->ret;
	m->fn(inst, args, &ret);
	switch (m->ret) {
	case TRS_T_VOID: break;
	case TRS_T_BOOL: *(uint8_t *)r_ret = (uint8_t)(ret.b != 0); break;
	case TRS_T_INT: *(int64_t *)r_ret = ret.i; break;
	case TRS_T_FLOAT: *(double *)r_ret = ret.f; break;
	case TRS_T_STRING:
		destroy_string(r_ret);
		p_string_new_with_utf8_chars_and_len2(r_ret, ret.s ? ret.s : "", ret.s ? (int64_t)strlen(ret.s) : 0);
		break;
	case TRS_T_F64ARR:
		destroy_f64arr(r_ret);
		write_f64arr(r_ret, &ret);
		break;
	}
	free_val(&ret);
	for (int i = 0; i < m->nargs; i++)
		free_val(&args[i]);
}

static void method_call(void *ud, GDExtensionClassInstancePtr inst, const GDExtensionConstVariantPtr *p_args,
		GDExtensionInt argc, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	const trs_method_def *m = ud;
	r_error->error = GDEXTENSION_CALL_OK;
	if (argc != m->nargs) {
		r_error->error = argc < m->nargs ? GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error->expected = m->nargs;
		return;
	}
	trs_val args[TRS_MAX_ARGS], ret;
	memset(args, 0, sizeof(args));
	memset(&ret, 0, sizeof(ret));
	for (int i = 0; i < m->nargs; i++) {
		GDExtensionVariantPtr v = (GDExtensionVariantPtr)p_args[i];
		GDExtensionVariantType vt = p_variant_get_type(v), want = gd_type(m->args[i]);
		args[i].type = m->args[i];
		if (vt == GDEXTENSION_VARIANT_TYPE_INT && want == GDEXTENSION_VARIANT_TYPE_FLOAT) {
			int64_t x;
			from_variant[GDEXTENSION_VARIANT_TYPE_INT](&x, v);
			args[i].f = (double)x;
			continue;
		}
		if (vt != want) {
			for (int j = 0; j < i; j++)
				free_val(&args[j]);
			r_error->error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
			r_error->argument = i;
			r_error->expected = want;
			return;
		}
		opaque tmp;
		memset(&tmp, 0, sizeof(tmp));
		switch (m->args[i]) {
		case TRS_T_BOOL: {
			uint8_t b;
			from_variant[want](&b, v);
			args[i].b = b != 0;
		} break;
		case TRS_T_INT: from_variant[want](&args[i].i, v); break;
		case TRS_T_FLOAT: from_variant[want](&args[i].f, v); break;
		case TRS_T_STRING:
			from_variant[want](&tmp, v);
			read_string(&tmp, &args[i]);
			destroy_string(&tmp);
			break;
		case TRS_T_F64ARR:
			from_variant[want](&tmp, v);
			read_f64arr(&tmp, &args[i]);
			destroy_f64arr(&tmp);
			break;
		case TRS_T_VOID: break;
		}
	}
	ret.type = m->ret;
	m->fn(inst, args, &ret);
	if (m->ret != TRS_T_VOID) {
		GDExtensionVariantType rt = gd_type(m->ret);
		opaque tmp;
		memset(&tmp, 0, sizeof(tmp));
		uint8_t b;
		void *src = &tmp;
		switch (m->ret) {
		case TRS_T_BOOL: b = (uint8_t)(ret.b != 0); src = &b; break;
		case TRS_T_INT: src = &ret.i; break;
		case TRS_T_FLOAT: src = &ret.f; break;
		case TRS_T_STRING: str_new(&tmp, ret.s ? ret.s : "", ret.s ? (int64_t)strlen(ret.s) : 0); break;
		case TRS_T_F64ARR: write_f64arr(&tmp, &ret); break;
		case TRS_T_VOID: break;
		}
		p_variant_destroy(r_return);
		to_variant[rt](r_return, src);
		if (m->ret == TRS_T_STRING)
			destroy_string(&tmp);
		else if (m->ret == TRS_T_F64ARR)
			destroy_f64arr(&tmp);
	}
	free_val(&ret);
	for (int i = 0; i < m->nargs; i++)
		free_val(&args[i]);
}

/* ---- class registration ------------------------------------------------- */

typedef struct {
	const trs_class_def *def;
	opaque name, parent;
} class_rec;

static GDExtensionObjectPtr create_instance(void *ud, GDExtensionBool notify_postinitialize) {
	class_rec *rec = ud;
	GDExtensionObjectPtr obj = p_classdb_construct_object3(&rec->parent);
	void *self = rec->def->create();
	p_object_set_instance(obj, &rec->name, self);
	if (notify_postinitialize && mb_notification) {
		int64_t what = 0; /* NOTIFICATION_POSTINITIALIZE */
		uint8_t reversed = 0;
		const void *args[2] = { &what, &reversed };
		p_object_method_bind_ptrcall(mb_notification, obj, args, NULL);
	}
	return obj;
}

static void free_instance(void *ud, GDExtensionClassInstancePtr inst) {
	class_rec *rec = ud;
	rec->def->destroy(inst);
}

static void fill_prop(GDExtensionPropertyInfo *pi, trs_type t, const char *name, opaque *sn_name, opaque *sn_cls, opaque *s_hint) {
	sn_new(sn_name, name ? name : "");
	sn_new(sn_cls, "");
	str_new(s_hint, "", 0);
	pi->type = gd_type(t);
	pi->name = sn_name;
	pi->class_name = sn_cls;
	pi->hint = 0;
	pi->hint_string = s_hint;
	pi->usage = 6; /* PROPERTY_USAGE_DEFAULT */
}

static GDExtensionClassMethodArgumentMetadata meta_of(trs_type t) {
	if (t == TRS_T_INT)
		return GDEXTENSION_METHOD_ARGUMENT_METADATA_INT_IS_INT64;
	if (t == TRS_T_FLOAT)
		return GDEXTENSION_METHOD_ARGUMENT_METADATA_REAL_IS_DOUBLE;
	return GDEXTENSION_METHOD_ARGUMENT_METADATA_NONE;
}

void trs_binder_register(const trs_class_def *cls) {
	binder_late_init();
	class_rec *rec = calloc(1, sizeof(*rec)); /* lives for the process */
	rec->def = cls;
	sn_new(&rec->name, cls->name);
	sn_new(&rec->parent, cls->parent);

	GDExtensionClassCreationInfo6 info;
	memset(&info, 0, sizeof(info));
	info.is_exposed = 1;
	info.create_instance_func = create_instance;
	info.free_instance_func = free_instance;
	info.class_userdata = rec;
	p_classdb_register_extension_class6(g_lib, &rec->name, &rec->parent, &info);

	for (int k = 0; k < cls->nmethods; k++) {
		const trs_method_def *m = &cls->methods[k];
		opaque mname, rn, rc, rh;
		opaque an[TRS_MAX_ARGS], ac[TRS_MAX_ARGS], ah[TRS_MAX_ARGS];
		GDExtensionPropertyInfo ret_info, arg_info[TRS_MAX_ARGS];
		GDExtensionClassMethodArgumentMetadata arg_meta[TRS_MAX_ARGS];
		sn_new(&mname, m->name);
		fill_prop(&ret_info, m->ret, "", &rn, &rc, &rh);
		for (int i = 0; i < m->nargs; i++) {
			fill_prop(&arg_info[i], m->args[i], m->arg_names[i], &an[i], &ac[i], &ah[i]);
			arg_meta[i] = meta_of(m->args[i]);
		}
		GDExtensionClassMethodInfo mi;
		memset(&mi, 0, sizeof(mi));
		mi.name = &mname;
		mi.method_userdata = (void *)m;
		mi.call_func = method_call;
		mi.ptrcall_func = method_ptrcall;
		mi.method_flags = GDEXTENSION_METHOD_FLAGS_DEFAULT;
		mi.has_return_value = m->ret != TRS_T_VOID;
		mi.return_value_info = &ret_info;
		mi.return_value_metadata = meta_of(m->ret);
		mi.argument_count = (uint32_t)m->nargs;
		mi.arguments_info = arg_info;
		mi.arguments_metadata = arg_meta;
		p_classdb_register_extension_class_method(g_lib, &rec->name, &mi);
		/* StringNames/Strings above are intentionally not destroyed: they are
		 * tiny, registration happens once, and Godot holds its own copies. */
	}
}
