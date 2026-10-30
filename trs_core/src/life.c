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

/* Life on the ground: where a body's plants grow and its animals roam.
 * The surface is cut into cells (cube-sphere patches about CELL_M across)
 * and each cell's plants and herds follow from a hash of the cell alone,
 * so they are the same every visit, from any distance, whoever asks. A
 * cell holds density x its true area of candidates per kind, each kept
 * only where that kind can live (biome, slope, height), and clear of the
 * launch site (plants of all its flat, animals of the pad and runway) and
 * the relics. Everything stands on the terrain's own
 * height. It is scenery: nothing here touches the physics. */
#include "trs/system.h"
#include "trs/terrain.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define CELL_M 400.0      /* target cell size along the surface */
#define SITE_CLEAR 300.0  /* m of bare ground round the launch site, past its flat */
/* animals roam the flat too (a penguin out on the pad meets them a short
 * walk off), but keep off the pad itself and the runway: a strip east of
 * the pad from 380 m to 1500 m (scripts/flight/launch_site.gd) */
#define PAD_CLEAR 500.0
#define RUNWAY_FROM 380.0
#define RUNWAY_TO 1500.0
#define RUNWAY_CLEAR 120.0  /* m either side of its middle */
#define RELIC_CLEAR 80.0  /* m round a relic */

static uint64_t mix(uint64_t x) {
	x += 0x9e3779b97f4a7c15ull;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
	return x ^ (x >> 31);
}

typedef struct {
	uint64_t s;
} rng;

static double rnd(rng *r) {
	r->s = mix(r->s);
	return (double)(r->s >> 11) * (1.0 / 9007199254740992.0);
}

trs_patch_id trs_patch_from_dir(trs_v3 d, int level) {
	double ax = fabs(d.x), ay = fabs(d.y), az = fabs(d.z), a, b;
	int face;
	if (ax >= ay && ax >= az) {
		face = d.x > 0 ? 0 : 1;
		a = face == 0 ? d.y / ax : d.z / ax;
		b = face == 0 ? d.z / ax : d.y / ax;
	} else if (ay >= az) {
		face = d.y > 0 ? 2 : 3;
		a = face == 2 ? d.z / ay : d.x / ay;
		b = face == 2 ? d.x / ay : d.z / ay;
	} else {
		face = d.z > 0 ? 4 : 5;
		a = face == 4 ? d.x / az : d.y / az;
		b = face == 4 ? d.y / az : d.x / az;
	}
	int n = 1 << level;
	int i = (int)floor((a + 1) * 0.5 * n), j = (int)floor((b + 1) * 0.5 * n);
	trs_patch_id p = { face, level, i < 0 ? 0 : (i >= n ? n - 1 : i), j < 0 ? 0 : (j >= n ? n - 1 : j) };
	return p;
}

int trs_life_level(const trs_body *b) {
	int l = (int)lround(log2(b->radius * (TRS_PI / 2) / CELL_M));
	return l < 0 ? 0 : (l > 20 ? 20 : l);
}

static trs_v3 ground(const trs_body *b, trs_v3 d) {
	return v3_scale(d, b->radius + (b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, d) : 0));
}

/* area of the spherical triangle a b c (unit vectors), steradians */
static double tri_area(trs_v3 a, trs_v3 b, trs_v3 c) {
	return 2 * atan2(fabs(v3_dot(a, v3_cross(b, c))), 1 + v3_dot(a, b) + v3_dot(b, c) + v3_dot(c, a));
}

double trs_life_cell_area(const trs_body *b, trs_patch_id p) {
	trs_v3 a = trs_patch_dir(p, 0, 0), c = trs_patch_dir(p, 1, 0), d = trs_patch_dir(p, 1, 1), e = trs_patch_dir(p, 0, 1);
	return (tri_area(a, c, d) + tri_area(a, d, e)) * b->radius * b->radius;
}

/* Is direction d too near the launch site for kind L? */
static int by_site(const trs_body *b, const trs_life *L, trs_v3 d) {
	double m = acos(fmin(1, v3_dot(d, b->terrain.site_dir))) * b->radius;
	if (!L->fauna)
		return m < b->terrain.site_radius + SITE_CLEAR;
	if (m < PAD_CLEAR)
		return 1;
	trs_v3 up = b->terrain.site_dir, e = v3_cross(v3(0, 0, 1), up);
	e = v3_len2(e) < 1e-20 ? v3(0, 1, 0) : v3_norm(e);
	trs_v3 n = v3_cross(up, e);
	double x = v3_dot(d, e) * b->radius, y = v3_dot(d, n) * b->radius;
	return v3_dot(d, up) > 0 && x > RUNWAY_FROM - RUNWAY_CLEAR && x < RUNWAY_TO + RUNWAY_CLEAR && fabs(y) < RUNWAY_CLEAR;
}

/* Can kind L live at direction d? (n: the ground's normal there) */
int trs_life_fits(const trs_body *b, const trs_life *L, trs_v3 d, trs_v3 *n) {
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, d) : 0;
	if (h < L->height_min || h > L->height_max)
		return 0;
	if (L->biomes) {
		int bi = trs_body_biome(b, d);
		if (bi < 0 || !(L->biomes & (1u << bi)))
			return 0;
	}
	if (b->terrain.has_site && by_site(b, L, d))
		return 0;
	for (int k = 0; k < b->nrelics; k++)
		if (acos(fmin(1, v3_dot(d, b->relics[k].dir))) * b->radius < RELIC_CLEAR)
			return 0;
	*n = b->terrain.enabled ? trs_terrain_normal(&b->terrain, b->radius, d, 2.0) : d;
	return v3_dot(*n, d) >= L->min_up;
}

/* a direction `m` metres from d, toward angle a in d's tangent plane */
static trs_v3 offset_dir(const trs_body *b, trs_v3 d, double a, double m) {
	trs_v3 e = v3_cross(v3(0, 0, 1), d);
	if (v3_len2(e) < 1e-20)
		e = v3(0, 1, 0);
	e = v3_norm(e);
	trs_v3 n = v3_cross(d, e);
	double th = m / b->radius;
	trs_v3 t = v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a)));
	return v3_norm(v3_add(v3_scale(d, cos(th)), v3_scale(t, sin(th))));
}

int trs_life_cell(const trs_body *b, trs_patch_id p, unsigned kinds, trs_v3 *centre, trs_life_instance *out, int max) {
	trs_v3 cd = trs_patch_dir(p, 0.5, 0.5);
	trs_v3 c = ground(b, cd);
	*centre = c;
	double km2 = trs_life_cell_area(b, p) / 1e6;
	int n = 0;
	for (int k = 0; k < b->nlife; k++) {
		if (!(kinds & (1u << k)))
			continue;
		const trs_life *L = &b->life[k];
		rng r = { mix(((uint64_t)p.face << 58) ^ ((uint64_t)p.level << 52) ^ ((uint64_t)p.i << 26) ^ (uint64_t)p.j ^ mix((uint64_t)k + 7919)) };
		double want = L->density * km2;
		int count = (int)floor(want + rnd(&r));
		for (int q = 0; q < count; q++) {
			trs_v3 d = trs_patch_dir(p, rnd(&r), rnd(&r));
			double s0 = rnd(&r), y0 = rnd(&r);
			trs_v3 nrm;
			int herd = L->herd > 0 ? L->herd : 1; /* (a grove, for plants) */
			if (!trs_life_fits(b, L, d, &nrm))
				continue;
			for (int m = 0; m < herd && n < max; m++) {
				trs_v3 dm = d, nm = nrm;
				double s = s0, y = y0;
				if (m > 0) {
					dm = offset_dir(b, d, rnd(&r) * 2 * TRS_PI, L->spread * sqrt(rnd(&r)));
					s = rnd(&r), y = rnd(&r);
					if (!trs_life_fits(b, L, dm, &nm))
						continue;
				}
				trs_life_instance *o = &out[n++];
				memset(o, 0, sizeof *o);
				o->kind = k;
				o->pos = v3_sub(v3_add(ground(b, dm), v3_scale(dm, L->fly)), c);
				o->up = nm;
				o->scale = s;
				o->yaw = y;
				if (L->fauna && L->loop > 0) {
					/* its loop: round its own spot, on (or over) the ground */
					double a0 = rnd(&r) * 2 * TRS_PI;
					o->nloop = TRS_LIFE_LOOP;
					for (int e = 0; e < TRS_LIFE_LOOP; e++) {
						double rr = L->loop * (0.6 + 0.4 * rnd(&r));
						trs_v3 de = offset_dir(b, dm, a0 + 2 * TRS_PI * e / TRS_LIFE_LOOP, rr);
						o->loop[e] = v3_sub(v3_add(ground(b, de), v3_scale(de, L->fly)), c);
					}
				}
			}
		}
	}
	return n;
}

int trs_life_cells_near(const trs_body *b, trs_v3 eye_dir, double range, trs_patch_id *out, int max) {
	int level = trs_life_level(b), n = 0;
	double cell = b->radius * (TRS_PI / 2) / (1 << level);
	/* (a third of a cell apart: by a cube corner cells shrink to about
	 * 0.58 of their size, and none may slip between the samples) */
	int steps = (int)ceil(range / (cell / 3));
	for (int x = -steps; x <= steps; x++)
		for (int y = -steps; y <= steps; y++) {
			double dx = x * cell / 3, dy = y * cell / 3, dist = hypot(dx, dy);
			if (dist > range + cell)
				continue;
			trs_v3 d = dist > 0 ? offset_dir(b, eye_dir, atan2(dy, dx), dist) : eye_dir;
			trs_patch_id p = trs_patch_from_dir(d, level);
			int seen = 0;
			for (int k = 0; k < n && !seen; k++)
				seen = out[k].face == p.face && out[k].i == p.i && out[k].j == p.j;
			if (!seen && n < max)
				out[n++] = p;
		}
	return n;
}
