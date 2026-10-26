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

/* Closest approach of two predicted paths (patches a and b) between t0 and
 * t1, wherever both are around the same body at the same time. */
typedef struct {
	double t, dist, rel_speed;
	int body;
	trs_v3 ra, rb; /* positions relative to that body at t */
} trs_approach;
/* Returns 1 and fills *out if the paths share a body somewhere in [t0, t1]. */
int trs_closest_approach(const trs_patch *a, int na, const trs_patch *b, int nb, double t0, double t1, trs_approach *out);

/* A Hohmann transfer between sibling bodies (circular, coplanar model):
 * phases in rad (how far `to` leads `from`), times in s, speeds in m/s. */
typedef struct {
	double phase_best, phase_now, wait, synodic, flight_time, v_inf, dv_eject;
} trs_transfer;
/* park_r: radius of a circular parking orbit around `from` for dv_eject
 * (0: skip). Returns 0 if the two don't share a parent. */
int trs_transfer_window(const trs_system *sys, int from, int to, double t, double park_r, trs_transfer *out);
/* A burn (maneuver node) from orbit o around `body` that carries the craft
 * into planet `to`'s sphere of influence, aiming for a periapsis of
 * want_pe_alt there: searched around the next transfer window. Returns 1
 * if the path gets there (*pe_alt then its periapsis altitude). */
int trs_plan_transfer(const trs_system *sys, int body, const trs_orbit *o, double t_now, int to, double want_pe_alt,
		trs_node *out, double *pe_alt);
/* A small correction burn a minute after t_now (on the way, around the
 * star) that brings the periapsis at `to` to want_pe_alt, looking ahead
 * `horizon` s. Returns 1 if the path then enters `to`'s sphere. */
int trs_plan_correction(const trs_system *sys, int body, const trs_orbit *o, double t_now, int to, double want_pe_alt,
		double horizon, trs_node *out, double *pe_alt);

#endif
