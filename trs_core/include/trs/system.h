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

#define TRS_MAX_BODIES 32

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
	/* stars: several systems can share one world, each star fixed at its
	 * galactic position (m, the home star at the origin, shared axes);
	 * every body knows its star. A star's soi is its heliopause, past which
	 * a ship is between the stars; luminosity scales solar power. */
	int star;
	trs_v3 galactic;
	double luminosity;
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
/* Biome under body-fixed direction dir (unit), or -1 if the body has none. */
int trs_body_biome(const trs_body *b, trs_v3 dir);
/* Atmosphere at altitude (m above surface): pressure (Pa), density (kg/m^3). */
void trs_atmosphere_at(const trs_body *b, double altitude, double *pressure, double *density);

#endif
