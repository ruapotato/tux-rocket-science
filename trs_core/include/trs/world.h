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

/* The world: time, bodies, and vessels, plus the two integration modes.
 *
 *   PHYSICS  fixed 50 Hz RK4 in doubles, used under thrust or in atmosphere.
 *   RAILS    analytic Kepler from an epoch state; drift-free, warpable.
 *
 * Vessel states are always relative to their current SOI body, in a
 * non-rotating frame centred on that body (patched conics). */
#ifndef TRS_WORLD_H
#define TRS_WORLD_H

#include "trs/kepler.h"
#include "trs/system.h"

#define TRS_PHYSICS_HZ 50
#define TRS_PHYSICS_DT (1.0 / TRS_PHYSICS_HZ)

/* SOI hysteresis: exit slightly outside the sphere, enter slightly inside,
 * so a vessel sitting on the boundary can never ping-pong. */
#define TRS_SOI_EXIT_FACTOR (1.0 + 1e-7)
#define TRS_SOI_ENTER_FACTOR (1.0 - 1e-7)

typedef enum {
	TRS_MODE_PHYSICS = 0,
	TRS_MODE_RAILS = 1,
} trs_mode;

typedef enum {
	TRS_EV_NONE = 0,
	TRS_EV_SOI_EXIT,    /* leaves current body's SOI into its parent */
	TRS_EV_SOI_ENTER,   /* enters a child body's SOI */
	TRS_EV_ATMOSPHERE,  /* reaches the top of the atmosphere (inbound) */
	TRS_EV_SURFACE,     /* reaches the surface of an airless body */
} trs_event_kind;

typedef struct {
	trs_event_kind kind;
	double t;
	int body; /* body the vessel is in after the event (or that it hits) */
} trs_event;

/* Force model hook for physics mode (thrust, drag, ...). Returns the
 * non-gravitational acceleration at the given state. M2 plugs vessels in. */
typedef trs_v3 (*trs_accel_fn)(void *user, int body, double t, trs_v3 r, trs_v3 v);

typedef struct {
	int alive;
	int body;
	trs_mode mode;
	trs_v3 r, v;      /* authoritative in PHYSICS mode */
	trs_orbit orbit;  /* authoritative in RAILS mode */
	trs_accel_fn accel;
	void *accel_user;
	int soi_changes;
} trs_vessel;

#define TRS_MAX_VESSELS 64

typedef struct {
	trs_system sys;
	double t, t_comp; /* world time with Kahan compensation */
	trs_vessel vessels[TRS_MAX_VESSELS];
	int nvessels;
} trs_world;

void trs_world_init(trs_world *w, const trs_system *sys, double t0);
int trs_world_add_vessel(trs_world *w, int body, trs_v3 r, trs_v3 v);
void trs_world_remove_vessel(trs_world *w, int id);

/* Current state relative to the vessel's SOI body, whatever its mode. */
void trs_vessel_state(const trs_world *w, int id, trs_v3 *r, trs_v3 *v);
/* Osculating orbit at the current time, whatever its mode. */
void trs_vessel_orbit(const trs_world *w, int id, trs_orbit *o);
void trs_vessel_set_mode(trs_world *w, int id, trs_mode mode);
void trs_vessel_set_state(trs_world *w, int id, int body, trs_v3 r, trs_v3 v);

/* One fixed physics tick for every PHYSICS vessel (rails vessels just ride
 * along analytically). Handles SOI transitions at tick boundaries. */
void trs_world_step_physics(trs_world *w);

/* Advance by dt on rails. Every vessel must be on rails. Stops early when
 * vessel `stop_on` (or -1 for none) hits atmosphere/surface; returns that
 * event in *stop (kind NONE when the full dt elapsed). */
void trs_world_warp(trs_world *w, double dt, int stop_on, trs_event *stop);

/* Next patched-conic event for an orbit around `body`, in [t, t_max]. */
trs_event trs_next_event(const trs_system *sys, int body, const trs_orbit *o, double t, double t_max);

/* Re-expresses a state from `from` body to `to` body (parent or child). */
void trs_rebase_state(const trs_system *sys, int from, int to, double t, trs_v3 *r, trs_v3 *v);

#endif
