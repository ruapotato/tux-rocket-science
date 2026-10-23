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

#include "trs/terrain.h"

#include <math.h>
#include <string.h>

/* ---- gradient noise (improved Perlin) with a seeded permutation ---------- */

static unsigned hash3(int x, int y, int z, unsigned seed) {
	unsigned h = seed * 0x9E3779B1u;
	h ^= (unsigned)x * 0x85EBCA77u;
	h = (h << 13) | (h >> 19);
	h ^= (unsigned)y * 0xC2B2AE3Du;
	h = (h << 13) | (h >> 19);
	h ^= (unsigned)z * 0x27D4EB2Fu;
	h ^= h >> 16;
	h *= 0x7FEB352Du;
	h ^= h >> 15;
	h *= 0x846CA68Bu;
	h ^= h >> 16;
	return h;
}

static double grad(unsigned h, double x, double y, double z) {
	switch (h & 15) {
	case 0: return x + y;
	case 1: return -x + y;
	case 2: return x - y;
	case 3: return -x - y;
	case 4: return x + z;
	case 5: return -x + z;
	case 6: return x - z;
	case 7: return -x - z;
	case 8: return y + z;
	case 9: return -y + z;
	case 10: return y - z;
	case 11: return -y - z;
	case 12: return x + y;
	case 13: return -x + y;
	case 14: return -y + z;
	default: return -y - z;
	}
}

static double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
static double lerp(double a, double b, double t) { return a + (b - a) * t; }

static double noise3(double x, double y, double z, unsigned seed) {
	double fx = floor(x), fy = floor(y), fz = floor(z);
	int X = (int)fx, Y = (int)fy, Z = (int)fz;
	x -= fx, y -= fy, z -= fz;
	double u = fade(x), v = fade(y), w = fade(z);
	double n000 = grad(hash3(X, Y, Z, seed), x, y, z);
	double n100 = grad(hash3(X + 1, Y, Z, seed), x - 1, y, z);
	double n010 = grad(hash3(X, Y + 1, Z, seed), x, y - 1, z);
	double n110 = grad(hash3(X + 1, Y + 1, Z, seed), x - 1, y - 1, z);
	double n001 = grad(hash3(X, Y, Z + 1, seed), x, y, z - 1);
	double n101 = grad(hash3(X + 1, Y, Z + 1, seed), x - 1, y, z - 1);
	double n011 = grad(hash3(X, Y + 1, Z + 1, seed), x, y - 1, z - 1);
	double n111 = grad(hash3(X + 1, Y + 1, Z + 1, seed), x - 1, y - 1, z - 1);
	return lerp(lerp(lerp(n000, n100, u), lerp(n010, n110, u), v), lerp(lerp(n001, n101, u), lerp(n011, n111, u), v), w);
}

static double unit_rand(unsigned h) {
	return (h & 0xFFFFFF) / 16777216.0;
}

/* Bowl-with-rim crater field on a 3D cell grid around the unit sphere. */
static double craters(const trs_terrain *t, trs_v3 d) {
	double f = t->crater_freq;
	double x = d.x * f, y = d.y * f, z = d.z * f;
	int cx = (int)floor(x), cy = (int)floor(y), cz = (int)floor(z);
	double h = 0;
	for (int dx = -1; dx <= 1; dx++)
		for (int dy = -1; dy <= 1; dy++)
			for (int dz = -1; dz <= 1; dz++) {
				int X = cx + dx, Y = cy + dy, Z = cz + dz;
				unsigned s = hash3(X, Y, Z, t->seed ^ 0xC0FFEEu);
				if ((s & 3) != 0) /* 1 in 4 cells holds a crater */
					continue;
				double px = X + unit_rand(hash3(X, Y, Z, s)), py = Y + unit_rand(hash3(Y, Z, X, s)),
					   pz = Z + unit_rand(hash3(Z, X, Y, s));
				double rad = 0.15 + 0.35 * unit_rand(s >> 3);
				double dist = sqrt((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz)) / rad;
				if (dist >= 1.6)
					continue;
				double depth = t->crater_depth * (0.4 + 0.6 * unit_rand(s >> 7)) * rad;
				if (dist < 1.0)
					h += depth * (dist * dist - 1.0) + depth * 0.35 * dist * dist * dist * dist; /* bowl */
				else {
					double r = (dist - 1.0) / 0.6; /* rim falling off outside */
					h += depth * 0.35 * (1 - r) * (1 - r);
				}
			}
	return h;
}

double trs_terrain_height(const trs_terrain *t, double body_radius, trs_v3 d) {
	(void)body_radius;
	if (!t->enabled)
		return 0;
	double h = 0, amp = 1, freq = t->base_freq, norm = 0;
	for (int o = 0; o < t->octaves; o++) {
		double n = noise3(d.x * freq, d.y * freq, d.z * freq, t->seed + (unsigned)o * 101u);
		double r = 1.0 - fabs(n);
		h += amp * ((1 - t->ridge) * n + t->ridge * (r * r - 0.5));
		norm += amp;
		amp *= t->persistence;
		freq *= 2.0;
	}
	h = t->offset + t->amplitude * h / norm;
	if (t->crater_freq > 0)
		h += craters(t, d);
	if (t->has_site) {
		double ang = acos(fmax(-1.0, fmin(1.0, v3_dot(d, t->site_dir))));
		double dist = ang * body_radius;
		if (dist < t->site_radius * 4) {
			double s = (dist - t->site_radius) / (t->site_radius * 3);
			s = s < 0 ? 0 : s > 1 ? 1 : s;
			s = s * s * (3 - 2 * s);
			h = t->site_height + (h - t->site_height) * s;
		}
	}
	return h;
}

trs_v3 trs_terrain_normal(const trs_terrain *t, double R, trs_v3 d, double step) {
	trs_v3 a = fabs(d.z) < 0.9 ? v3(0, 0, 1) : v3(1, 0, 0);
	trs_v3 e1 = v3_norm(v3_cross(a, d)), e2 = v3_cross(d, e1);
	double ang = step / R;
	trs_v3 d1 = v3_norm(v3_madd(d, e1, ang)), d2 = v3_norm(v3_madd(d, e2, ang));
	trs_v3 d1m = v3_norm(v3_madd(d, e1, -ang)), d2m = v3_norm(v3_madd(d, e2, -ang));
	trs_v3 p1 = v3_scale(d1, R + trs_terrain_height(t, R, d1)), p1m = v3_scale(d1m, R + trs_terrain_height(t, R, d1m));
	trs_v3 p2 = v3_scale(d2, R + trs_terrain_height(t, R, d2)), p2m = v3_scale(d2m, R + trs_terrain_height(t, R, d2m));
	trs_v3 n = v3_norm(v3_cross(v3_sub(p1, p1m), v3_sub(p2, p2m)));
	return v3_dot(n, d) < 0 ? v3_neg(n) : n;
}

/* ---- cube sphere ------------------------------------------------------------- */

trs_v3 trs_patch_dir(trs_patch_id p, double u, double v) {
	double n = (double)(1 << p.level);
	double a = ((p.i + u) / n) * 2 - 1, b = ((p.j + v) / n) * 2 - 1;
	trs_v3 c;
	switch (p.face) {
	case 0: c = v3(1, a, b); break;
	case 1: c = v3(-1, b, a); break;
	case 2: c = v3(b, 1, a); break;
	case 3: c = v3(a, -1, b); break;
	case 4: c = v3(a, b, 1); break;
	default: c = v3(b, a, -1); break;
	}
	return v3_norm(c);
}

typedef struct {
	const trs_terrain *t;
	double R, split;
	trs_v3 eye;
	int max_level;
	trs_patch_id *out;
	int n, max;
} lod_ctx;

static void lod_rec(lod_ctx *cx, trs_patch_id p) {
	trs_v3 c = trs_patch_dir(p, 0.5, 0.5);
	double size = cx->R * (TRS_PI / 2) / (double)(1 << p.level);
	trs_v3 cp = v3_scale(c, cx->R + trs_terrain_height(cx->t, cx->R, c));
	double dist = v3_len(v3_sub(cx->eye, cp));
	if (p.level < cx->max_level && dist < size * cx->split && cx->n + 4 <= cx->max) {
		for (int k = 0; k < 4; k++) {
			trs_patch_id q = { p.face, p.level + 1, p.i * 2 + (k & 1), p.j * 2 + (k >> 1) };
			lod_rec(cx, q);
		}
		return;
	}
	/* cull patches on the far side of the horizon */
	double eye_r = v3_len(cx->eye);
	if (eye_r > cx->R) {
		double horizon = sqrt(eye_r * eye_r - cx->R * cx->R) + size * 1.5 + cx->R * 0.05;
		if (dist > horizon && p.level > 0)
			return;
	}
	if (cx->n < cx->max)
		cx->out[cx->n++] = p;
}

int trs_terrain_lod(const trs_terrain *t, double R, trs_v3 eye, int max_level, double split, trs_patch_id *out, int max) {
	lod_ctx cx = { t, R, split, eye, max_level, out, 0, max };
	for (int f = 0; f < 6; f++) {
		trs_patch_id p = { f, 0, 0, 0 };
		lod_rec(&cx, p);
	}
	return cx.n;
}

int trs_terrain_patch_mesh(const trs_terrain *t, double R, trs_patch_id p, int res, double *pos, double *nrm, trs_v3 *center) {
	trs_v3 cd = trs_patch_dir(p, 0.5, 0.5);
	trs_v3 c = v3_scale(cd, R + trs_terrain_height(t, R, cd));
	*center = c;
	double size = R * (TRS_PI / 2) / (double)(1 << p.level);
	double step = size / (res - 1);
	int k = 0;
	/* main grid, then a skirt ring hanging below the edges to hide cracks */
	for (int pass = 0; pass < 2; pass++) {
		for (int y = 0; y < res; y++)
			for (int x = 0; x < res; x++) {
				int edge = x == 0 || y == 0 || x == res - 1 || y == res - 1;
				if (pass == 1 && !edge)
					continue;
				trs_v3 d = trs_patch_dir(p, (double)x / (res - 1), (double)y / (res - 1));
				double h = trs_terrain_height(t, R, d) - (pass ? size * 0.02 : 0);
				trs_v3 v = v3_sub(v3_scale(d, R + h), c);
				trs_v3 n = trs_terrain_normal(t, R, d, step);
				pos[k * 3] = v.x, pos[k * 3 + 1] = v.y, pos[k * 3 + 2] = v.z;
				nrm[k * 3] = n.x, nrm[k * 3 + 1] = n.y, nrm[k * 3 + 2] = n.z;
				k++;
			}
	}
	return k;
}
