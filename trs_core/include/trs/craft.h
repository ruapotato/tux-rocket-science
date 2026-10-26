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

/* A craft is a tree of parts that flies as one rigid body. The same
 * structure serves the hangar (design-time readouts) and live vessels.
 *
 * Invariants: part 0 is the root (parent -1) and every part's parent index
 * is smaller than its own, so the array is always topologically ordered.
 * Vessel-local frame: +Y is "up" (the nose), matching the hangar. */
#ifndef TRS_CRAFT_H
#define TRS_CRAFT_H

#include "trs/json.h"
#include "trs/part.h"
#include "trs/quat.h"

typedef enum {
	TRS_CHUTE_STOWED = 0,
	TRS_CHUTE_ARMED,  /* staged, waiting for enough air */
	TRS_CHUTE_SEMI,
	TRS_CHUTE_FULL,
	TRS_CHUTE_CUT,
} trs_chute_state;

typedef struct {
	int def;              /* catalog index */
	int uid;              /* stable id for views; survives splits */
	int parent;           /* -1 for the root */
	char node[16];        /* own attach node ("" = surface attach) */
	char parent_node[16]; /* parent's node ("" = surface) */
	trs_v3 pos;           /* vessel-local position of the part origin */
	trs_quat rot;         /* vessel-local orientation */
	int stage;            /* activation stage, -1 = none */
	int sym;              /* symmetry group (0 = none), editor metadata */
	int groups;           /* action groups it belongs to: bit g-1 for group g (1..9) */
	double fuel;          /* remaining propellant, kg (liquid or solid) */
	double mono;          /* remaining monopropellant, kg */
	double ec;            /* stored electricity, kJ */
	double ore;           /* ore in the bin, kg */
	int running;          /* drills and refineries: switched on */
	double limit;         /* engines: thrust limit, fraction of full (set in the hangar) */
	int engine_on;
	int chute;
	int legs_deployed;
	int fired; /* decoupler already fired */
	double temp; /* K, skin temperature (runtime only, not saved) */
	double chute_cda; /* m^2, canopy drag area while it inflates (runtime only) */
	double rcs_fire[TRS_MAX_NOZZLES]; /* RCS nozzle throttles last tick (runtime only) */
	double fin_flap; /* fins: control flap angle last tick, rad, + swings the trailing edge toward +X (runtime only) */
	char dock_name[48]; /* docking ports: the vessel it brought in when it docked */
	int dock_wait_uid;  /* docking ports: won't latch onto this port again until clear of it */
} trs_part;

typedef struct {
	char name[64];
	char description[256];
	const trs_part_catalog *cat;
	trs_part *parts;
	int count, cap;
	int next_uid;
} trs_craft;

void trs_craft_init(trs_craft *c, const trs_part_catalog *cat);
void trs_craft_free(trs_craft *c);
void trs_craft_copy(trs_craft *dst, const trs_craft *src);
int trs_craft_add(trs_craft *c, int def, int parent, const char *node, const char *parent_node, trs_v3 pos,
		trs_quat rot, int stage, int sym);

int trs_craft_from_json(trs_craft *c, const trs_part_catalog *cat, const trs_json *j, char *err, size_t errlen);
trs_json *trs_craft_to_json(const trs_craft *c);

static inline const trs_part_def *trs_craft_def(const trs_craft *c, int i) {
	return &c->cat->defs[c->parts[i].def];
}

/* Engine part i's propellant flow at full throttle, after its thrust limit. */
double trs_part_mdot(const trs_craft *c, int i);

double trs_craft_mass(const trs_craft *c);
/* Monopropellant on board and its capacity, kg (the whole craft shares it). */
double trs_craft_mono(const trs_craft *c, double *capacity);
/* stored electricity (kJ) and capacity; adding (or drawing, < 0) spreads
 * over the batteries, clamped to empty and full: returns what moved */
double trs_craft_ec(const trs_craft *c, double *capacity);
double trs_craft_ec_add(trs_craft *c, double kj);
/* ore aboard (kg) and the bins' capacity; adding (or taking, < 0)
 * spreads over the bins, clamped: returns what moved */
double trs_craft_ore(const trs_craft *c, double *capacity);
double trs_craft_ore_add(trs_craft *c, double kg);
/* Removes `amount` kg of monopropellant evenly (proportionally) from every part holding some. */
void trs_craft_mono_draw(trs_craft *c, double amount);
double trs_craft_part_mass(const trs_craft *c, int i);
/* Mass, centre of mass (vessel-local) and inertia tensor about the CoM
 * in vessel-local axes. */
void trs_craft_mass_props(const trs_craft *c, double *mass, trs_v3 *cm, trs_m3 *inertia);
int trs_craft_max_stage(const trs_craft *c);

/* Fuel groups: connected parts not separated by a decoupler share liquid
 * propellant. group[i] receives a group id per part. Returns group count. */
int trs_craft_fuel_groups(const trs_craft *c, int *group);
double trs_craft_group_fuel(const trs_craft *c, const int *group, int g);
/* Removes `amount` kg evenly (proportionally) from group g's liquid tanks. */
void trs_craft_group_draw(trs_craft *c, const int *group, int g, double amount);

/* Fuel network. Groups (above) are linked by feed lines: a feed line takes
 * propellant from its own group into the group of the first part its outlet
 * reaches. A liquid engine draws from the farthest-upstream groups that
 * still hold propellant, split evenly between them, and only reaches its own
 * group once everything feeding it is dry, so side tanks piped into the core
 * empty first. Within a group, tanks drain proportionally. */
typedef struct {
	int count;           /* parts */
	int ngroups;
	int *group;          /* per part */
	int nlines;
	int *line_part, *line_target; /* per feed line part; target -1 = reaches nothing */
	unsigned char *up;   /* ngroups^2: up[a * ngroups + b] = a feeds b, directly or through others */
} trs_fuel_net;

void trs_fuel_net_build(trs_fuel_net *n, const trs_craft *c);
/* The part feed line i's outlet reaches (-1 = none): marching out along the
 * line's local +Z (away from the surface it sits on), the first part of
 * another group; the pipe runs past decouplers, which hold no propellant.
 * dist (optional) receives how far along it got. */
int trs_craft_feed_target(const trs_craft *c, const int *group, int i, double *dist);
void trs_fuel_net_free(trs_fuel_net *n);
/* Liquid propellant per group (gf[ngroups]). */
void trs_fuel_levels(const trs_fuel_net *n, const trs_craft *c, double *gf);
/* Groups an engine in group g draws from right now (out[ngroups]); returns the count. */
int trs_fuel_sources(const trs_fuel_net *n, const double *gf, int g, int *out);
/* Whether group a's propellant can reach group g (a == g, or upstream of it). */
int trs_fuel_reaches(const trs_fuel_net *n, int a, int g);
/* Liquid draw per group (kg/s) for per-part engine flows mdot[count] (0 =
 * not drawing); rate[ngroups] is overwritten. */
void trs_fuel_rates(const trs_fuel_net *n, const trs_craft *c, const double *mdot, double *rate);
/* Whether engine part i currently has propellant available. */
int trs_craft_engine_fed(const trs_craft *c, const trs_fuel_net *n, int i);

/* Splitting. For decoupler part d, moves the detached piece into `out`
 * (initialised here) and removes it from c. Returns 0 if nothing detached. */
int trs_craft_split(trs_craft *c, int d, trs_craft *out);
/* Destroys part d. Each of its children's subtrees becomes a separate craft
 * written to pieces[] (initialised here, up to max). If d is the root, the
 * remaining craft is left empty. Returns the number of pieces. */
int trs_craft_remove_part(trs_craft *c, int d, trs_craft *pieces, int max);
/* Marks (into mask) the parts that firing decoupler d would detach. */
void trs_craft_detach_set(const trs_craft *c, int d, unsigned char *mask);

/* Per-stage readouts computed exactly as the flight sim burns:
 * stage s fires, then engines burn until the engines that stage s-1 will
 * drop are dry (or, if it drops none, until every lit engine is dry). */
typedef struct {
	int stage;
	double mass_start, mass_end; /* kg */
	double dv_vac, dv_sl;        /* m/s */
	double twr_vac, twr_sl;      /* at stage start, against g_ref */
	double thrust_vac, thrust_sl; /* N, at stage start */
	double burn_time;            /* s at full throttle */
	double isp_vac, isp_sl;      /* effective, first segment */
} trs_stage_stats;

/* Runs the staging plan starting with `next_stage` (normally the max
 * stage for a fresh craft). Returns the number of entries written; stats[k]
 * is for stage next_stage - k. Pressure for "sl" figures is p_sl. */
int trs_craft_stage_stats(const trs_craft *c, int next_stage, double g_ref, double p_sl, trs_stage_stats *stats,
		int max);

/* Applies stage s to a craft at design time (ignite + chutes + decouple,
 * keeping only the root piece). Used by the readouts and the gates. */
void trs_craft_activate_stage_root_only(trs_craft *c, int s);

#endif
