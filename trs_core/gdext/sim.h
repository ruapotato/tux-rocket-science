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

/* Per-instance state behind the GDScript `TrsSim` object. */
#ifndef TRS_GDEXT_SIM_H
#define TRS_GDEXT_SIM_H

#include "binder.h"
#include "trs/part.h"
#include "trs/world.h"

#include <stdio.h>

typedef struct {
	trs_world *world; /* NULL until load_system() succeeds */
	trs_part_catalog catalog;
	int catalog_loaded;
	char err[256];
	void *plan_job; /* plan_start() snapshot for a background plan_run() */
} trs_sim_handle;

extern const trs_method_def TRS_SIM_WORLD_METHODS[];
extern const int TRS_SIM_WORLD_METHOD_COUNT;
extern const trs_method_def TRS_SIM_CRAFT_METHODS[];
extern const int TRS_SIM_CRAFT_METHOD_COUNT;
extern const trs_method_def TRS_SIM_FLIGHT_METHODS[];
extern const int TRS_SIM_FLIGHT_METHOD_COUNT;

#endif
