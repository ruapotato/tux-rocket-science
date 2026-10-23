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

/* Trajectory prediction: chains of patched conics, with maneuver nodes
 * applied as impulses in the orbit's prograde/normal/radial frame. */
#ifndef TRS_TRAJECTORY_H
#define TRS_TRAJECTORY_H

#include "trs/world.h"

typedef struct {
	double t;             /* absolute time of the burn */
	double pro, nrm, rad; /* delta-v components, m/s */
} trs_node;

typedef enum {
	TRS_PATCH_END_HORIZON = 0,
	TRS_PATCH_END_SOI_EXIT,
	TRS_PATCH_END_SOI_ENTER,
	TRS_PATCH_END_ATMOSPHERE,
	TRS_PATCH_END_SURFACE,
	TRS_PATCH_END_NODE,
} trs_patch_end;

typedef struct {
	int body;
	trs_orbit orbit;
	double t0, t1;
	trs_patch_end end;
	int node; /* node index that starts this patch, -1 if none */
} trs_patch;

/* Delta-v vector (SOI frame) for a node applied to an orbit at the node time. */
trs_v3 trs_node_vector(const trs_orbit *o, const trs_node *n);
/* Predicts up to max patches starting from orbit o around body at time t.
 * Nodes may be in any order. Returns the patch count. */
int trs_trajectory(const trs_system *sys, int body, const trs_orbit *o, double t, const trs_node *nodes, int nn,
		double horizon, trs_patch *out, int max);

#endif
