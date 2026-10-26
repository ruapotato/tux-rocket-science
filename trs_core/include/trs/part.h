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

/* Part definitions, loaded from the data/parts JSON catalogs. Nothing about a part is
 * hardcoded in C; this struct is just the parsed form of the data. */
#ifndef TRS_PART_H
#define TRS_PART_H

#include "trs/vec3.h"

#include <stddef.h>

#define TRS_G0 9.80665
#define TRS_P_REF 101325.0 /* pressure at which isp_sl is quoted */
#define TRS_MAX_NODES 6
#define TRS_MAX_NOZZLES 6 /* RCS nozzles per part */
#define TRS_T_AMBIENT 288.15 /* K: skin temperature at rest, and what skins radiate toward */

typedef enum { TRS_PROP_NONE = 0, TRS_PROP_LIQUID, TRS_PROP_SOLID } trs_propellant;
typedef enum { TRS_DECOUPLER_NONE = 0, TRS_DECOUPLER_STACK, TRS_DECOUPLER_RADIAL } trs_decoupler_kind;

typedef struct {
	char name[16];
	trs_v3 pos, dir;
	double radius;
} trs_attach_node;

typedef struct {
	char id[32];
	char name[64];
	char category[16];
	double mass;        /* dry, kg */
	double fuel_liquid; /* capacity, kg */
	double fuel_solid;  /* capacity, kg */
	double fuel_mono;   /* monopropellant capacity, kg (shared by the whole vessel) */
	int crew;
	double torque;      /* reaction torque, N m */
	double crash_tolerance; /* m/s */

	/* engine */
	trs_propellant propellant;
	double thrust_vac, isp_vac, isp_sl, gimbal; /* gimbal in radians */
	/* air-breathing (jet): thrust_vac is the static sea-level thrust; flow
	 * and thrust scale with air density / jet_rho_ref and fall off as
	 * 1 - (airspeed / jet_v_max)^2; nothing in vacuum */
	int air_breathing;
	double jet_v_max, jet_rho_ref;

	/* RCS: small monopropellant thrusters; each nozzle pushes opposite the
	 * way it points (part-local position and exhaust direction) */
	int rcs_n;
	double rcs_thrust, rcs_isp_vac, rcs_isp_sl; /* per nozzle, N in vacuum */
	trs_v3 rcs_pos[TRS_MAX_NOZZLES], rcs_dir[TRS_MAX_NOZZLES];

	/* docking port: its "top" node is the docking face */
	int is_dock;
	double dock_capture;   /* m: faces this close (and lined up, and slow) latch */
	double dock_max_speed; /* m/s closing speed it can take */
	double dock_max_angle; /* rad off face-to-face it can take */
	double dock_impulse;   /* N s pushing the two apart on undocking */

	/* decoupler */
	trs_decoupler_kind decoupler;
	double eject_impulse; /* N s */

	/* parachute */
	int is_parachute;
	double chute_semi_cda, chute_full_cda, chute_min_pressure, chute_max_speed;
	double chute_full_alt; /* full deployment height above the ground */
	double chute_inflate;  /* s for the canopy to open from nothing to full size */

	/* feed line: pipes propellant from its group to what its outlet reaches */
	double feed_reach; /* m along local +Z from the attach point (0 = not a feed line) */

	/* electricity, kJ and kW: a battery's store, a constant draw (probe
	 * cores, pods), what reaction torque costs at full use per kN m of it,
	 * a solar panel's output face-on to the sun at solar_ref_m from it (its
	 * face is local +Z, off the hull), and an antenna's cost to send home
	 * one point of research (it gets antenna_fraction of a result's worth) */
	double ec_capacity, ec_draw, torque_kw_per_knm, solar_kw, solar_ref_m;
	double antenna_kj_per_point, antenna_fraction;

	/* a penguin out on a walk (EVA): its feet carry it at up to eva_walk m/s
	 * over the ground (as far as friction lets them), it jumps off at
	 * eva_jump m/s, and it can climb back aboard a crewed part within
	 * eva_reach m of its hull */
	int is_eva;
	double eva_walk, eva_jump, eva_reach;
	int is_ladder; /* a penguin within reach of it can climb it to the hatch */

	/* wheel: a sprung foot wheel_length out along the
	 * part's +Z (off the hull), rolling along the craft's nose with
	 * mu_roll (mu_brake braked), gripping sideways with mu_side; wheels
	 * ahead of the centre of mass steer up to wheel_steer rad */
	int is_wheel;
	double wheel_length, wheel_mu_roll, wheel_mu_brake, wheel_mu_side, wheel_steer;

	/* landing leg */
	int is_leg;
	double leg_length, leg_spring, leg_damper, leg_max_speed;
	trs_v3 leg_hinge;          /* part-local hinge point */
	double leg_angle;          /* deployed swing-out angle from vertical, rad */
	double leg_stowed_fraction; /* telescoped length when stowed */

	/* aero: Cd per face and reference areas (axial = end face, side = profile) */
	double cd_top, cd_bottom, cd_side, area_axial, area_side;
	double cop;    /* cross-flow centre of pressure along the axis, fraction of height from the part centre */
	int plate;     /* flat plate (fins): cross-flow force only along the plate normal (local X) */
	double lift_slope; /* plates: normal-force slope dCN/dalpha per rad at small angles (fins lift) */
	double fin_deflect; /* plates: control deflection, rad (0 = fixed fin) */
	double fin_span;    /* plates: how far the fin stands off its root, m */
	double radius_top, radius_bottom; /* end radii: a narrow leading end is a nose */

	/* thermal: the hottest skin element (see heat() in vessel_physics.c) */
	double max_temp;    /* K: the part fails above this */
	double skin_heat;   /* J/(m^2 K): heat capacity per unit skin area */
	double emissivity;

	/* geometry used for mass properties and default areas */
	double radius, height;

	trs_attach_node nodes[TRS_MAX_NODES];
	int nnodes;
	int surface_attach;
} trs_part_def;

typedef struct {
	trs_part_def *defs;
	int count;
} trs_part_catalog;

int trs_catalog_load(trs_part_catalog *cat, const char *path, char *err, size_t errlen);
void trs_catalog_free(trs_part_catalog *cat);
int trs_catalog_find(const trs_part_catalog *cat, const char *id);
const trs_attach_node *trs_part_node(const trs_part_def *d, const char *name);

/* Engine helpers. mdot is independent of pressure; thrust scales with Isp. */
double trs_engine_isp(const trs_part_def *d, double pressure);
double trs_engine_mdot(const trs_part_def *d); /* at full throttle */
/* share of a jet's static sea-level flow (and thrust) at this air density
 * and airspeed; 1 for rockets */
double trs_engine_air_factor(const trs_part_def *d, double density, double airspeed);
/* Leg foot contact point (part-local) for deployment fraction k in [0, 1].
 * The visuals use the same formula, so feet touch where they are drawn. */
trs_v3 trs_leg_foot(const trs_part_def *d, double k);

#endif
