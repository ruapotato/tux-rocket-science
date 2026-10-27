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

/* GDExtension entry point. Exposes the simulation to GDScript as `TrsSim`.
 * Godot nodes are views: everything authoritative stays in here. */
#include "binder.h"
#include "sim.h"
#include "trs/trs_core.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define TRS_EXPORT __declspec(dllexport)
#else
#define TRS_EXPORT __attribute__((visibility("default")))
#endif

extern const trs_method_def TRS_COMPUTER_METHODS[];
extern const int TRS_COMPUTER_METHOD_COUNT;
void *trs_computer_create(void);
void trs_computer_destroy(void *self);

static void *sim_create(void) {
	return calloc(1, sizeof(trs_sim_handle));
}

static void sim_destroy(void *self) {
	trs_sim_handle *h = self;
	if (h->world) {
		trs_world_free(h->world);
		free(h->world);
	}
	if (h->catalog_loaded)
		trs_catalog_free(&h->catalog);
	free(h->plan_job);
	free(h);
}

/* JSON string escaping into a malloc()ed buffer (quotes included). */
static char *json_quote(const char *s) {
	size_t n = strlen(s), k = 0;
	char *o = malloc(n * 6 + 3);
	o[k++] = '"';
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		unsigned char c = *p;
		if (c == '"' || c == '\\') {
			o[k++] = '\\';
			o[k++] = (char)c;
		} else if (c == '\n') {
			o[k++] = '\\';
			o[k++] = 'n';
		} else if (c < 0x20) {
			static const char hex[] = "0123456789abcdef";
			memcpy(o + k, "\\u00", 4);
			k += 4;
			o[k++] = hex[c >> 4];
			o[k++] = hex[c & 15];
		} else {
			o[k++] = (char)c;
		}
	}
	o[k++] = '"';
	o[k] = 0;
	return o;
}

static void m_version(void *self, const trs_val *a, trs_val *r) {
	(void)self;
	(void)a;
	r->s = trs_strdup_printf("%s", trs_version_string());
}

/* run_gate(name, data_dir) -> JSON {"gate","status","milestone","log"} */
static void m_run_gate(void *self, const trs_val *a, trs_val *r) {
	(void)self;
	const trs_gate_def *g = trs_gate_find(a[0].s);
	if (!g) {
		r->s = trs_strdup_printf("{\"gate\":\"%s\",\"status\":\"UNKNOWN\",\"log\":\"\"}", "?");
		return;
	}
	if (!g->fn) {
		r->s = trs_strdup_printf("{\"gate\":\"%s\",\"status\":\"SCRIPTED\",\"milestone\":\"%s\",\"log\":\"\"}", g->name, g->milestone);
		return;
	}
	trs_log log = { 0 };
	trs_gate_status s = g->fn(&log, a[1].s);
	char *q = json_quote(log.buf ? log.buf : "");
	r->s = trs_strdup_printf("{\"gate\":\"%s\",\"status\":\"%s\",\"milestone\":\"%s\",\"log\":%s}", g->name,
			trs_gate_status_name(s), g->milestone, q);
	free(q);
	trs_log_free(&log);
}

/* gate_roster() -> JSON [{"gate","milestone","native"}] */
static void m_gate_roster(void *self, const trs_val *a, trs_val *r) {
	(void)self;
	(void)a;
	size_t n;
	const trs_gate_def *g = trs_gates(&n);
	trs_log out = { 0 };
	trs_log_printf(&out, "[");
	for (size_t i = 0; i < n; i++)
		trs_log_printf(&out, "%s{\"gate\":\"%s\",\"milestone\":\"%s\",\"native\":%s}", i ? "," : "", g[i].name,
				g[i].milestone, g[i].fn ? "true" : "false");
	trs_log_printf(&out, "]");
	r->s = out.buf; /* ownership passes to the binder */
}

/* Marshalling self-test used by the smoke gate: sum doubles and echo. */
static void m_echo_f64(void *self, const trs_val *a, trs_val *r) {
	(void)self;
	r->arr.n = a[0].arr.n + 1;
	r->arr.data = malloc(sizeof(double) * (size_t)r->arr.n);
	double sum = 0;
	for (int64_t i = 0; i < a[0].arr.n; i++) {
		r->arr.data[i] = a[0].arr.data[i];
		sum += a[0].arr.data[i];
	}
	r->arr.data[a[0].arr.n] = sum;
}

static const trs_method_def SIM_METHODS[] = {
	{ "version", m_version, TRS_T_STRING, 0, { 0 }, { 0 } },
	{ "run_gate", m_run_gate, TRS_T_STRING, 2, { TRS_T_STRING, TRS_T_STRING }, { "gate", "data_dir" } },
	{ "gate_roster", m_gate_roster, TRS_T_STRING, 0, { 0 }, { 0 } },
	{ "echo_f64", m_echo_f64, TRS_T_F64ARR, 1, { TRS_T_F64ARR }, { "values" } },
};

static void initialize(void *ud, GDExtensionInitializationLevel level) {
	(void)ud;
	if (level != GDEXTENSION_INITIALIZATION_SCENE)
		return;
	/* One method table per API area, merged into the single TrsSim class. */
	static trs_method_def all[128];
	int n = 0;
	for (size_t i = 0; i < sizeof(SIM_METHODS) / sizeof(SIM_METHODS[0]); i++)
		all[n++] = SIM_METHODS[i];
	for (int i = 0; i < TRS_SIM_WORLD_METHOD_COUNT; i++)
		all[n++] = TRS_SIM_WORLD_METHODS[i];
	for (int i = 0; i < TRS_SIM_CRAFT_METHOD_COUNT; i++)
		all[n++] = TRS_SIM_CRAFT_METHODS[i];
	for (int i = 0; i < TRS_SIM_FLIGHT_METHOD_COUNT; i++)
		all[n++] = TRS_SIM_FLIGHT_METHODS[i];
	static trs_class_def sim_class;
	sim_class = (trs_class_def){ "TrsSim", "RefCounted", sim_create, sim_destroy, all, n };
	trs_binder_register(&sim_class);
	/* the ship's computer (os/), its own class */
	static trs_class_def computer_class;
	computer_class = (trs_class_def){ "TrsComputer", "RefCounted", trs_computer_create, trs_computer_destroy,
		TRS_COMPUTER_METHODS, TRS_COMPUTER_METHOD_COUNT };
	trs_binder_register(&computer_class);
}

static void deinitialize(void *ud, GDExtensionInitializationLevel level) {
	(void)ud;
	(void)level;
}

TRS_EXPORT GDExtensionBool trs_core_init(GDExtensionInterfaceGetProcAddress gpa, GDExtensionClassLibraryPtr lib,
		GDExtensionInitialization *init) {
	if (!trs_binder_init(gpa, lib))
		return 0;
	init->minimum_initialization_level = GDEXTENSION_INITIALIZATION_SCENE;
	init->userdata = NULL;
	init->initialize = initialize;
	init->deinitialize = deinitialize;
	return 1;
}
