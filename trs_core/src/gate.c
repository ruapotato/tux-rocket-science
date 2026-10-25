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

#include "trs/gate.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void trs_log_printf(trs_log *log, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	va_list ap2;
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) {
		va_end(ap2);
		return;
	}
	if (log->len + (size_t)n + 1 > log->cap) {
		size_t cap = log->cap ? log->cap : 256;
		while (cap < log->len + (size_t)n + 1)
			cap *= 2;
		char *nb = realloc(log->buf, cap);
		if (!nb) {
			va_end(ap2);
			return;
		}
		log->buf = nb;
		log->cap = cap;
	}
	vsnprintf(log->buf + log->len, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	log->len += (size_t)n;
}

void trs_log_free(trs_log *log) {
	free(log->buf);
	log->buf = NULL;
	log->len = log->cap = 0;
}

const char *trs_gate_status_name(trs_gate_status s) {
	switch (s) {
	case TRS_GATE_PASS: return "PASS";
	case TRS_GATE_FAIL: return "FAIL";
	case TRS_GATE_PENDING: return "PENDING";
	}
	return "?";
}

static trs_gate_status gate_smoke(trs_log *log, const char *data_dir) {
	(void)data_dir;
	volatile double a = 0.1, b = 0.2;
	double c = a + b;
	trs_log_printf(log, "trs_core alive; double check 0.1+0.2=%.17g\n", c);
	return (sizeof(double) == 8 && c == 0.30000000000000004 && c != 0.3) ? TRS_GATE_PASS : TRS_GATE_FAIL;
}

trs_gate_status trs_gate_kepler(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_mode_switch(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_patched_conics(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_deltav(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_staging(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_aero(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_landing(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_heating(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_collision(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_rcs(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_persist(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_docking(trs_log *log, const char *data_dir);
trs_gate_status trs_gate_fins(trs_log *log, const char *data_dir);

static const trs_gate_def GATES[] = {
	{ "test-smoke", "M0", gate_smoke },
	{ "test-kepler", "M1", trs_gate_kepler },
	{ "test-mode-switch", "M1", trs_gate_mode_switch },
	{ "test-patched-conics", "M1", trs_gate_patched_conics },
	{ "test-deltav", "M2", trs_gate_deltav },
	{ "test-staging", "M2", trs_gate_staging },
	{ "test-aero", "M2", trs_gate_aero },
	{ "test-landing", "M4", trs_gate_landing },
	{ "test-heating", "M7", trs_gate_heating },
	{ "test-collision", "M8", trs_gate_collision },
	{ "test-rcs", "M9", trs_gate_rcs },
	{ "test-persist", "M10", trs_gate_persist },
	{ "test-docking", "M11", trs_gate_docking },
	{ "test-fins", "M12", trs_gate_fins },
	{ "test-craft-roundtrip", "M3", NULL },
	{ "test-ascent", "M4", NULL },
	{ "test-moon", "M5", NULL },
	{ "test-world", "M10", NULL },
	{ "test-warp", "M13", NULL },
	{ "screens", "M6", NULL },
};

const trs_gate_def *trs_gates(size_t *count) {
	*count = sizeof(GATES) / sizeof(GATES[0]);
	return GATES;
}

const trs_gate_def *trs_gate_find(const char *name) {
	size_t n;
	const trs_gate_def *g = trs_gates(&n);
	for (size_t i = 0; i < n; i++)
		if (strcmp(g[i].name, name) == 0)
			return &g[i];
	return NULL;
}
