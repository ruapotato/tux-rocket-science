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

static double smooth01(double x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return x * x * (3 - 2 * x); }

/* What a landmark adds to the ground's height h at d (0 away from it). */
static double feature_height(const struct trs_feature *f, double R, trs_v3 d, unsigned seed, double h) {
	double c = v3_dot(d, f->dir);
	if (c < 0)
		return 0;
	double dist = acos(fmin(1.0, c)) * R;
	/* a ragged edge: no landmark is a perfect circle */
	/* (its noise in units of the feature's own size, so a crater on a small
	 * moon is as smooth as one on a planet) */
	double sz = R / fmax(f->kind == TRS_FEAT_CANYON ? f->width * 4 : f->radius, 1.0);
	double wob = 1.0 + 0.12 * noise3(d.x * sz * 1.5, d.y * sz * 1.5, d.z * sz * 1.5, seed) + 0.04 * noise3(d.x * sz * 5, d.y * sz * 5, d.z * sz * 5, seed + 1);
	switch (f->kind) {
	case TRS_FEAT_VOLCANO: {
		double x = dist / (f->radius * wob);
		if (x >= 1.3)
			return 0;
		/* concave flanks, gullied, up to the rim of a flat-floored caldera */
		double cone = pow(fmax(0.0, 1.0 - x), 1.7) * smooth01((1.3 - x) / 0.3);
		double gully = 1.0 - 0.08 * fabs(noise3(d.x * sz * 12, d.y * sz * 12, d.z * sz * 12, seed + 2)) * smooth01(x * 4);
		double up = f->height * cone * gully;
		double xc = 0.13;
		if (x < xc) {
			double rim = f->height * pow(1.0 - xc, 1.7);
			up = rim - f->depth * (1.0 - pow(x / xc, 6.0));
		}
		return up;
	}
	case TRS_FEAT_CRATER: {
		double x = dist / (f->radius * wob);
		if (x >= 2.2)
			return 0;
		/* the bowl, its floor flattened by what fell back in; the rim thrown
		 * up around it, fading out over the ejecta; a central peak */
		double bowl = x < 1.0 ? -f->depth * (1.0 - pow(x, 2.4)) : 0;
		double rim = f->height * exp(-pow((x - 1.0) / (x < 1.0 ? 0.12 : 0.35), 2.0));
		double peak = x < 0.22 ? f->depth * 0.45 * pow(1.0 - x / 0.22, 1.5) : 0;
		/* inside, the ground's own roughness is mostly gone */
		double calm = x < 1.0 ? -h * 0.6 * (1.0 - smooth01((x - 0.8) / 0.2)) : 0;
		return bowl + rim + peak + calm;
	}
	case TRS_FEAT_CANYON: {
		/* how far along the canyon's great circle, and how far off it */
		double along = atan2(v3_dot(d, f->t), c) * R;
		if (fabs(along) > f->length + f->width * 4)
			return 0;
		double off = asin(fmax(-1.0, fmin(1.0, v3_dot(d, f->g)))) * R;
		/* it wanders */
		off += f->width * 0.9 * sin(along / (f->length * 0.21) + 1.3) + f->width * 0.4 * sin(along / (f->length * 0.067));
		double w = f->width * (0.8 + 0.4 * (0.5 + 0.5 * noise3(d.x * sz * 0.6, d.y * sz * 0.6, d.z * sz * 0.6, seed + 3)));
		double y = fabs(off) / w;
		if (y >= 1.6)
			return 0;
		/* steep walls, a narrow floor, benches part way down; shallower toward its ends */
		double wall = 1.0 - smooth01((y - 0.25) / 0.75);
		double bench = 0.12 * smooth01((y - 0.45) / 0.1) * (1.0 - smooth01((y - 0.6) / 0.1));
		double ends = smooth01((f->length - fabs(along)) / (f->width * 3));
		/* the plateau it cuts lifts a little at its lips */
		double lip = 0.08 * exp(-pow((y - 1.15) / 0.2, 2.0));
		return f->depth * ends * (-(wall - bench) + lip);
	}
	case TRS_FEAT_MESA: {
		double x = dist / (f->radius * wob);
		if (x >= 1.4)
			return 0;
		/* a flat top, cliffs, a scree apron */
		double top = 1.0 - smooth01((x - 0.86) / 0.08);
		double apron = 0.22 * (1.0 - smooth01((x - 0.94) / 0.4));
		/* (the top is smoothed out of the ground's own roughness, gradually:
		 * a step there showed as teeth along the cliff) */
		return f->height * fmax(top, apron * smooth01((1.4 - x) / 0.2)) - h * 0.7 * (1.0 - smooth01((x - 0.8) / 0.12));
	}
	}
	return 0;
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
	if (t->mountain_amplitude > 0) {
		/* mask: where ranges are (low frequency), ridges: sharp crests */
		double mask = noise3(d.x * 1.7, d.y * 1.7, d.z * 1.7, t->seed + 7001u) * 0.5 + 0.5;
		mask += noise3(d.x * 4.1, d.y * 4.1, d.z * 4.1, t->seed + 7002u) * 0.25;
		double lo = 1.0 - t->mountain_coverage;
		double m = (mask - lo) / 0.18;
		m = m < 0 ? 0 : m > 1 ? 1 : m;
		m = m * m * (3 - 2 * m);
		if (m > 0) {
			double ridge = 0, a = 1, f = t->mountain_freq, wsum = 0, prev = 1;
			for (int o = 0; o < 9; o++) {
				double n = 1.0 - fabs(noise3(d.x * f, d.y * f, d.z * f, t->seed + 9000u + (unsigned)o * 37u));
				n = n * n;
				ridge += n * a * prev; /* weight by the previous octave: crisp peaks, smooth valleys */
				wsum += a;
				prev = n;
				a *= 0.5;
				f *= 2.07;
			}
			h += t->mountain_amplitude * m * ridge / wsum;
		}
	}
	if (t->hills_amplitude > 0) {
		/* hills: rounded crests, warped so they run in no grid, in regions
		 * (where the mask is low the plain stays plain) */
		double f = body_radius / t->hills_size;
		unsigned s = t->seed + 8100u;
		double wx = noise3(d.x * f * 0.35, d.y * f * 0.35, d.z * f * 0.35, s + 1);
		double wy = noise3(d.x * f * 0.35 + 5.2, d.y * f * 0.35, d.z * f * 0.35, s + 2);
		double wz = noise3(d.x * f * 0.35, d.y * f * 0.35 + 9.1, d.z * f * 0.35, s + 3);
		double qx = d.x * f + wx * 1.4, qy = d.y * f + wy * 1.4, qz = d.z * f + wz * 1.4;
		double sum = 0, a = 1, ws = 0;
		for (int o = 0; o < 5; o++) {
			double n = noise3(qx, qy, qz, s + 10u + (unsigned)o * 17u);
			double b = 1.0 - fabs(n);
			sum += a * (0.55 * b * b + 0.45 * (n * 0.5 + 0.5));
			ws += a;
			a *= 0.45;
			qx *= 2.03;
			qy *= 2.03;
			qz *= 2.03;
		}
		double fm = f / 14.0;
		double mask = noise3(d.x * fm, d.y * fm, d.z * fm, s + 4) * 0.5 + 0.5 + 0.2 * noise3(d.x * fm * 3.1, d.y * fm * 3.1, d.z * fm * 3.1, s + 5);
		double m = smooth01((mask - (1.0 - t->hills_coverage)) / 0.22);
		h += t->hills_amplitude * m * (sum / ws - 0.5);
	}
	for (int r = 0; r < t->nranges; r++) {
		double ang = acos(fmax(-1.0, fmin(1.0, v3_dot(d, t->range_dir[r]))));
		double x = ang * body_radius / t->range_radius[r];
		if (x < 2.5) {
			double env = exp(-x * x * 1.6);
			double ridge = 0, a = 1, f = t->mountain_freq > 0 ? t->mountain_freq * 1.3 : 50, wsum = 0, prev = 1;
			for (int o = 0; o < 9; o++) {
				double n = 1.0 - fabs(noise3(d.x * f, d.y * f, d.z * f, t->seed + 9500u + (unsigned)(r * 13 + o) * 41u));
				n = n * n;
				ridge += n * a * prev;
				wsum += a;
				prev = n;
				a *= 0.5;
				f *= 2.07;
			}
			h += t->range_height[r] * env * (0.35 + 0.65 * ridge / wsum);
		}
	}
	if (t->crater_freq > 0) {
		/* a cascade of ever smaller, shallower crater fields */
		trs_terrain tt = *t;
		for (int k = 0; k < 5; k++) {
			h += craters(&tt, d);
			tt.crater_freq *= 3.1;
			tt.crater_depth /= 3.4;
			tt.seed += 977u;
		}
	}
	for (int k = 0; k < t->nfeatures; k++)
		h += feature_height(&t->feature[k], body_radius, d, t->seed + 31u * (unsigned)k, h);
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
	/* cull patches beyond the horizon, counting the tallest terrain that
	 * could still poke up over it (otherwise distant peaks leave gaps) */
	double eye_r = v3_len(cx->eye);
	if (eye_r > cx->R) {
		double hmax = cx->t->offset + cx->t->amplitude + cx->t->mountain_amplitude + 500.0;
		for (int r = 0; r < cx->t->nranges; r++)
			hmax += cx->t->range_height[r];
		double rt = cx->R + hmax;
		double horizon = sqrt(eye_r * eye_r - cx->R * cx->R) + sqrt(rt * rt - cx->R * cx->R) + size * 1.5;
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
