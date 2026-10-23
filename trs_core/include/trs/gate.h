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

/* Verification gates. The same gate code runs from the standalone test
 * binary and from `godot --headless -- --<gate>` through the extension. */
#ifndef TRS_GATE_H
#define TRS_GATE_H

#include <stddef.h>

typedef enum {
	TRS_GATE_PASS = 0,
	TRS_GATE_FAIL = 1,
	TRS_GATE_PENDING = 2, /* not implemented yet; reported, never counted as pass */
} trs_gate_status;

typedef struct {
	char *buf;
	size_t len, cap;
} trs_log;

void trs_log_printf(trs_log *log, const char *fmt, ...)
#if defined(__GNUC__)
	__attribute__((format(printf, 2, 3)))
#endif
	;
void trs_log_free(trs_log *log);

typedef trs_gate_status (*trs_gate_fn)(trs_log *log, const char *data_dir);

typedef struct {
	const char *name;      /* e.g. "test-kepler" */
	const char *milestone; /* milestone that turns it green */
	trs_gate_fn fn;        /* NULL when the gate is driven from GDScript */
} trs_gate_def;

/* Gates implemented in C. GDScript-driven gates (ascent, moon, screens)
 * are listed too with fn == NULL so every runner sees the same roster. */
const trs_gate_def *trs_gates(size_t *count);
const trs_gate_def *trs_gate_find(const char *name);
const char *trs_gate_status_name(trs_gate_status s);

#endif
