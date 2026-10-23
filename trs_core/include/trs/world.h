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

#include "trs/craft.h"
#include "trs/kepler.h"
#include "trs/quat.h"
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
	TRS_MODE_LANDED = 2, /* fixed to the rotating surface (clamps, landed warp) */
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

typedef enum {
	TRS_SAS_OFF = 0,
	TRS_SAS_STABILITY, /* hold the attitude captured when input is released */
	TRS_SAS_TARGET,    /* point the nose (+Y) along sas_target (SOI frame) */
	TRS_SAS_PROGRADE,  /* orbital velocity */
	TRS_SAS_RETROGRADE,
	TRS_SAS_NORMAL,
	TRS_SAS_ANTINORMAL,
	TRS_SAS_RADIAL_OUT,
	TRS_SAS_RADIAL_IN,
	TRS_SAS_SURF_PROGRADE, /* surface-relative velocity */
	TRS_SAS_SURF_RETROGRADE,
	TRS_SAS_COUNT
} trs_sas_mode;


typedef struct {
	int alive;
	int body;
	trs_mode mode;
	trs_v3 r, v;      /* centre of mass; authoritative in PHYSICS mode */
	trs_orbit orbit;  /* authoritative in RAILS mode */
	trs_accel_fn accel; /* optional extra force (tests); NULL normally */
	void *accel_user;
	int soi_changes;

	/* Rigid assembly. NULL = point mass (used by orbit-only tests). */
	trs_craft *craft;
	trs_quat q;       /* vessel-local -> SOI frame */
	trs_v3 w;         /* angular velocity, SOI frame, rad/s */
	double throttle;  /* 0..1, liquid engines */
	trs_v3 input;     /* pilot: pitch (about local X), yaw (local Z), roll (local Y), -1..1 */
	trs_sas_mode sas;
	trs_quat sas_hold;
	trs_v3 sas_target;
	int next_stage;   /* stage fired by the next trs_world_stage() */

	/* Telemetry (true numbers for the HUD) */
	trs_v3 sensed_accel; /* non-gravitational acceleration last tick, SOI frame */
	double dv_expended;  /* integral of |thrust accel| dt */
	double pressure, density, altitude;

	/* Surface */
	trs_v3 fixed_r;   /* body-fixed position in LANDED mode */
	trs_quat fixed_q; /* body-fixed attitude in LANDED mode */
	int clamped;      /* held by launch clamps until the first stage */
	int contacts;     /* contact points touching the ground last tick */
	int landed;       /* resting on the surface (contacts, slow) */
	double max_impact; /* worst contact speed seen, m/s (telemetry) */
	int npending; /* parts (by uid) to destroy after this tick */
	int pending_uid[64];
	double pending_speed[64];

	/* Caches rebuilt when the assembly changes */
	int dirty;
	double mass;
	trs_v3 cm;        /* vessel-local */
	trs_m3 inertia;   /* about cm, vessel-local */
	int *fuel_group;
	double *face_top, *face_bottom; /* exposed axial area per part */
	int ncontact;
	trs_v3 *contact_pt;  /* vessel-local contact points */
	int *contact_part;
	unsigned char *contact_leg;
	double bound_radius; /* max distance of any contact point from the CoM */
} trs_vessel;

typedef enum {
	TRS_WEV_PART_DESTROYED = 1,
	TRS_WEV_VESSEL_CREATED,
	TRS_WEV_SOI_CHANGE,
	TRS_WEV_LANDED,
	TRS_WEV_LIFTOFF,
	TRS_WEV_FLAMEOUT,
} trs_world_event_kind;

typedef struct {
	trs_world_event_kind kind;
	double t;
	int vessel;  /* vessel involved */
	int other;   /* new vessel id / body index */
	int uid;     /* part uid */
	char part[32];
	double speed;
} trs_world_event;

#define TRS_MAX_EVENTS 256

#define TRS_MAX_VESSELS 64
#define TRS_MAX_PENDING 64

typedef struct {
	trs_system sys;
	double t, t_comp; /* world time with Kahan compensation */
	trs_vessel vessels[TRS_MAX_VESSELS];
	int nvessels;
	trs_world_event events[TRS_MAX_EVENTS];
	int nevents;
	int next_uid; /* part uids are unique across the whole world */
} trs_world;

void trs_world_init(trs_world *w, const trs_system *sys, double t0);
void trs_world_free(trs_world *w);
int trs_world_add_vessel(trs_world *w, int body, trs_v3 r, trs_v3 v);
void trs_world_remove_vessel(trs_world *w, int id);
/* Adds a vessel built from a craft (copied). Position/velocity are for the
 * centre of mass; q orients the craft's local frame. Starts in PHYSICS. */
int trs_world_add_craft(trs_world *w, const trs_craft *c, int body, trs_v3 r, trs_v3 v, trs_quat q);
/* Places a craft on a body's surface at lat/lon (radians), nose up, held by
 * launch clamps until its first stage fires. Returns the vessel id. */
int trs_world_launch(trs_world *w, const trs_craft *c, int body, double lat, double lon);
void trs_world_push_event(trs_world *w, trs_world_event ev);
/* Body-frame attitude quaternion (rotation of the body since epoch). */
trs_quat trs_body_quat(const trs_body *b, double t);
/* Direction for a SAS mode at the vessel's current state (SOI frame). */
trs_v3 trs_sas_direction(const trs_world *w, int id, trs_sas_mode m);
/* Fires the vessel's next stage. New vessels created by decouplers are
 * written to new_ids (up to max); returns how many. */
int trs_world_stage(trs_world *w, int id, int *new_ids, int max);
/* Refreshes mass properties and caches after editing vessel->craft. */
void trs_vessel_refresh(trs_world *w, int id);
/* Linear and angular momentum (about the SOI body centre) of one vessel. */
void trs_vessel_momentum(const trs_world *w, int id, trs_v3 *p, trs_v3 *L);

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
