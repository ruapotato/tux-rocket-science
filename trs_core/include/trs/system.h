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

/* The fixed star system: bodies on analytic Kepler rails, loaded from JSON. */
#ifndef TRS_SYSTEM_H
#define TRS_SYSTEM_H

#include "trs/kepler.h"
#include "trs/terrain.h"

#include <stddef.h>

#define TRS_MAX_BODIES 64

typedef struct {
	double height;      /* top of atmosphere above the surface, m; 0 = airless */
	double p0;          /* sea-level static pressure, Pa */
	double rho0;        /* sea-level density, kg/m^3 */
	double scale_height; /* m */
} trs_atmosphere;

#define TRS_MAX_BIOMES 12

/* A named region of a body's surface. A point belongs to the first biome
 * in the list whose every condition holds; a biome with none catches the
 * rest. */
typedef struct {
	char id[32];
	char name[48];
	double abs_lat_min, abs_lat_max; /* rad, |latitude| */
	double height_min, height_max;   /* m, terrain height */
	int has_near;                    /* within near_radius of near_dir */
	trs_v3 near_dir;
	double near_radius; /* m, along the surface */
} trs_biome;

#define TRS_MAX_LIFE 12

/* Something that grows or lives on a body's surface (life.c): `density`
 * groups per square km of `herd` (a grove of plants, a herd of animals)
 * within `spread` m of the group's middle; an animal also
 * wanders a loop `loop` m round its own spot, `fly` m above the ground
 * if it flies. Only in the biomes in `biomes` (a bit per biome; 0 = any),
 * on ground no steeper than acos(min_up) from level and between height_min
 * and height_max (m of terrain height). How it looks is the game's
 * business (the same entry in the data). */
typedef struct {
	char id[24];
	int fauna;
	double density;
	unsigned biomes;
	double min_up;
	double height_min, height_max;
	int herd;
	double spread, loop, fly;
} trs_life;

#define TRS_MAX_RELICS 4

/* Something left on a body's surface by whoever came before: it hums on a
 * radio band a Signal Scanner hears. dir is body-fixed (unit); it stands
 * on the ground there. */
typedef struct {
	char id[32];
	char name[48];
	trs_v3 dir;
} trs_relic;

/* Where a relic is from a point: straight-line distance (m), distance
 * along the surface between the two ground points (m), the initial
 * great-circle heading to it (rad, clockwise from north), and whether it is
 * above the point's horizon (nothing between them but space). */
typedef struct {
	double distance, surface_distance, bearing;
	int in_sight;
} trs_relic_view;

typedef struct {
	char id[32];
	char name[48];
	int parent; /* -1 for the root star */
	double mu, radius, soi;
	double rotation_period; /* sidereal, s; spin axis is +Z */
	double rotation_at_epoch; /* rad at t = 0 */
	trs_atmosphere atm;
	trs_terrain terrain;
	int has_site; /* launch site */
	char site_name[48];
	double site_lat, site_lon, site_alt; /* rad, rad, m */
	trs_orbit orbit; /* relative to parent; unused for the root */
	double ore; /* ore abundance, 0..1: scales what drills dig */
	trs_biome biomes[TRS_MAX_BIOMES];
	int nbiomes;
	trs_relic relics[TRS_MAX_RELICS];
	int nrelics;
	trs_life life[TRS_MAX_LIFE];
	int nlife;
	/* stars: several systems can share one world, each star fixed at its
	 * galactic position (m, the home star at the origin, shared axes);
	 * every body knows its star. A star's soi is its heliopause, past which
	 * a ship is between the stars; luminosity scales solar power. */
	int star;
	trs_v3 galactic;
	double luminosity;
	double temperature; /* a star's surface, K (its colour) */
	/* a gas giant has no ground: its radius is the cloud tops, its air
	 * thickens without end below them, and a craft deeper than crush_depth
	 * (m under the cloud tops) is crushed */
	int gas_giant;
	double crush_depth;
	int children[TRS_MAX_BODIES];
	int nchildren;
} trs_body;

typedef struct {
	char name[64];
	trs_body bodies[TRS_MAX_BODIES];
	int count;
} trs_system;

/* Loads a system file and the "neighbours" it lists (other system files,
 * paths relative to it), each placed at its "galactic_position_ly". */
int trs_system_load(trs_system *sys, const char *path, char *err, size_t errlen);
int trs_system_load_json(trs_system *sys, const char *text, char *err, size_t errlen);
int trs_system_find(const trs_system *sys, const char *id);

/* State of body i relative to its parent (zero for the root). */
void trs_body_state(const trs_system *sys, int i, double t, trs_v3 *r, trs_v3 *v);
/* State of body i relative to the root star. */
void trs_body_state_abs(const trs_system *sys, int i, double t, trs_v3 *r, trs_v3 *v);
/* Body rotation angle (rad) about +Z at time t. */
double trs_body_rotation(const trs_body *b, double t);
/* Angular velocity vector of the body's spin. */
trs_v3 trs_body_omega(const trs_body *b);
/* Body-fixed <-> SOI-frame (non-rotating) conversions at time t. */
trs_v3 trs_body_to_fixed(const trs_body *b, double t, trs_v3 v);
trs_v3 trs_body_from_fixed(const trs_body *b, double t, trs_v3 v);
/* Terrain height (m above datum) under an SOI-frame position at time t. */
double trs_body_terrain_at(const trs_body *b, double t, trs_v3 r);
/* Position and velocity relative to the home star's galactic origin. */
void trs_body_state_galactic(const trs_system *sys, int i, double t, trs_v3 *r, trs_v3 *v);
/* Position and velocity relative to star `star` (galactic axes). Within
 * one system this never goes near the huge numbers of the galactic origin:
 * a double there (4e16 m out) resolves only metres. */
void trs_body_state_from(const trs_system *sys, int i, int star, double t, trs_v3 *r, trs_v3 *v);
/* Biome under body-fixed direction dir (unit), or -1 if the body has none. */
int trs_body_biome(const trs_body *b, trs_v3 dir);
/* Relic k of body b seen from body-fixed position p (m). */
trs_relic_view trs_relic_from(const trs_body *b, int k, trs_v3 p);
/* Relic k's body-fixed position: on the ground, at its terrain height. */
trs_v3 trs_relic_position(const trs_body *b, int k);
/* Life on the ground (life.c). One plant, or one animal of a herd, in a
 * cell: body-fixed position relative to the cell's centre (on the ground,
 * or `fly` over it), the ground's normal, random 0..1 picks for its size
 * and heading, and an animal's wander loop (relative to the cell centre). */
#define TRS_LIFE_LOOP 8
typedef struct {
	int kind;
	trs_v3 pos, up;
	double scale, yaw;
	int nloop;
	trs_v3 loop[TRS_LIFE_LOOP];
} trs_life_instance;
/* The cube-sphere patch level life is scattered on for body b (cells
 * about 400 m across), and the patch at that level under a direction. */
int trs_life_level(const trs_body *b);
trs_patch_id trs_patch_from_dir(trs_v3 dir, int level);
/* A patch's area on the body's sphere, m^2. */
double trs_life_cell_area(const trs_body *b, trs_patch_id p);
/* Whether kind L can live at body-fixed direction dir (and the ground's
 * normal there into n). */
int trs_life_fits(const trs_body *b, const trs_life *L, trs_v3 dir, trs_v3 *n);
/* The kinds in `kinds` (a bit per kind) living in cell p: writes up to
 * max, returns how many; the cell's centre (on the ground, body-fixed)
 * into centre. Each kind's are the same whichever others are asked for. */
int trs_life_cell(const trs_body *b, trs_patch_id p, unsigned kinds, trs_v3 *centre, trs_life_instance *out, int max);
/* The cells within `range` m (along the ground) of the point under eye_dir. */
int trs_life_cells_near(const trs_body *b, trs_v3 eye_dir, double range, trs_patch_id *out, int max);
/* Atmosphere at altitude (m above surface): pressure (Pa), density (kg/m^3). */
void trs_atmosphere_at(const trs_body *b, double altitude, double *pressure, double *density);

#endif
