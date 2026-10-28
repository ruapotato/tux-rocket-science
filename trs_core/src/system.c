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

#include "trs/system.h"
#include "trs/json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEG (TRS_PI / 180.0)

int trs_system_find(const trs_system *sys, const char *id) {
	for (int i = 0; i < sys->count; i++)
		if (!strcmp(sys->bodies[i].id, id))
			return i;
	return -1;
}

/* Appends a system file's bodies (the first world's file first). */
static int add_bodies(trs_system *sys, const trs_json *root, char *err, size_t errlen) {
	const trs_json *bodies = trs_json_get(root, "bodies");
	if (!bodies || bodies->type != TRS_JSON_ARRAY || bodies->count == 0) {
		snprintf(err, errlen, "system has no bodies");
		return 0;
	}
	if (sys->count + (int)bodies->count > TRS_MAX_BODIES) {
		snprintf(err, errlen, "too many bodies");
		return 0;
	}
	const trs_json *gp = trs_json_get(root, "galactic_position_ly");
#define AT(k) (trs_json_at(gp, k) && trs_json_at(gp, k)->type == TRS_JSON_NUMBER ? trs_json_at(gp, k)->num : 0.0)
	trs_v3 gal = v3(0, 0, 0);
	if (gp)
		gal = v3_scale(v3(AT(0), AT(1), AT(2)), 9.4607304725808e15);
#undef AT
	/* Bodies must be listed parent-first. */
	for (size_t k = 0; k < bodies->count; k++) {
		const trs_json *j = trs_json_at(bodies, k);
		trs_body *b = &sys->bodies[sys->count];
		snprintf(b->id, sizeof b->id, "%s", trs_json_str(j, "id", ""));
		snprintf(b->name, sizeof b->name, "%s", trs_json_str(j, "name", b->id));
		b->mu = trs_json_num(j, "mu", 0);
		b->radius = trs_json_num(j, "radius", 0);
		b->rotation_period = trs_json_num(j, "rotation_period", 0);
		b->rotation_at_epoch = trs_json_num(j, "rotation_at_epoch_deg", 0) * DEG;
		if (!b->id[0] || b->mu <= 0 || b->radius <= 0) {
			snprintf(err, errlen, "body %zu: id, mu and radius are required", k);
			return 0;
		}
		const char *pid = trs_json_str(j, "parent", NULL);
		b->parent = pid ? trs_system_find(sys, pid) : -1;
		if (pid && b->parent < 0) {
			snprintf(err, errlen, "body %s: parent %s must be listed first", b->id, pid);
			return 0;
		}
		if (b->parent < 0 && strcmp(trs_json_str(j, "kind", ""), "star") && sys->count > 0) {
			snprintf(err, errlen, "body %s: only stars may be parentless", b->id);
			return 0;
		}
		b->star = b->parent < 0 ? sys->count : sys->bodies[b->parent].star;
		b->galactic = gal;
		b->luminosity = trs_json_num(j, "luminosity", 1.0);
		const trs_json *atm = trs_json_get(j, "atmosphere");
		if (atm) {
			b->atm.height = trs_json_num(atm, "height", 0);
			b->atm.p0 = trs_json_num(atm, "sea_level_pressure", 0);
			b->atm.rho0 = trs_json_num(atm, "sea_level_density", 0);
			b->atm.scale_height = trs_json_num(atm, "scale_height", 1);
		}
		const trs_json *tj = trs_json_get(j, "terrain");
		if (tj) {
			trs_terrain *t = &b->terrain;
			t->enabled = 1;
			t->seed = (unsigned)trs_json_num(tj, "seed", 1);
			t->amplitude = trs_json_num(tj, "amplitude", 0);
			t->base_freq = trs_json_num(tj, "base_freq", 4);
			t->octaves = (int)trs_json_num(tj, "octaves", 6);
			t->persistence = trs_json_num(tj, "persistence", 0.5);
			t->offset = trs_json_num(tj, "offset", 0);
			t->ridge = trs_json_num(tj, "ridge", 0);
			t->crater_freq = trs_json_num(tj, "crater_freq", 0);
			t->crater_depth = trs_json_num(tj, "crater_depth", 0);
			t->mountain_amplitude = trs_json_num(tj, "mountain_amplitude", 0);
			t->mountain_freq = trs_json_num(tj, "mountain_freq", 40);
			t->mountain_coverage = trs_json_num(tj, "mountain_coverage", 0.3);
			t->hills_amplitude = trs_json_num(tj, "hills", 0);
			t->hills_size = trs_json_num(tj, "hills_km", 4) * 1000.0;
			t->hills_coverage = trs_json_num(tj, "hills_coverage", 0.6);
			const trs_json *rg = trs_json_get(tj, "ranges");
			for (size_t k = 0; rg && k < rg->count && t->nranges < 8; k++) {
				const trs_json *r = trs_json_at(rg, k);
				double la = trs_json_num(r, "lat_deg", 0) * DEG, lo = trs_json_num(r, "lon_deg", 0) * DEG;
				t->range_dir[t->nranges] = v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la));
				t->range_radius[t->nranges] = trs_json_num(r, "radius_km", 20) * 1000.0;
				t->range_height[t->nranges] = trs_json_num(r, "height", 2000);
				t->nranges++;
			}
		}
		/* landmarks: their shapes into the ground (their names and words
		 * are the game's, from the same data) */
		const trs_json *lmj = trs_json_get(j, "landmarks");
		for (size_t k = 0; lmj && b->terrain.enabled && k < lmj->count && b->terrain.nfeatures < 12; k++) {
			const trs_json *e = trs_json_at(lmj, k);
			const char *kind = trs_json_str(e, "kind", "");
			int kd = !strcmp(kind, "volcano") ? TRS_FEAT_VOLCANO : !strcmp(kind, "crater") ? TRS_FEAT_CRATER
			       : !strcmp(kind, "canyon") ? TRS_FEAT_CANYON : !strcmp(kind, "mesa") ? TRS_FEAT_MESA : 0;
			if (!kd)
				continue;
			struct trs_feature *f = &b->terrain.feature[b->terrain.nfeatures++];
			double la = trs_json_num(e, "lat_deg", 0) * DEG, lo = trs_json_num(e, "lon_deg", 0) * DEG;
			f->kind = kd;
			f->dir = v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la));
			f->radius = trs_json_num(e, "radius_km", 10) * 1000.0;
			f->height = trs_json_num(e, "height", 0);
			f->depth = trs_json_num(e, "depth", 0);
			f->length = trs_json_num(e, "length_km", 50) * 1000.0 / 2.0;
			f->width = trs_json_num(e, "width_km", 3) * 1000.0 / 2.0;
			/* its way at the centre: the bearing, clockwise from north */
			double br = trs_json_num(e, "bearing_deg", 90) * DEG;
			trs_v3 east = v3(-sin(lo), cos(lo), 0), north = v3(-sin(la) * cos(lo), -sin(la) * sin(lo), cos(la));
			f->t = v3_add(v3_scale(north, cos(br)), v3_scale(east, sin(br)));
			f->g = v3_norm(v3_cross(f->dir, f->t));
		}
		b->ore = trs_json_num(j, "ore", 0);
		const trs_json *bj = trs_json_get(j, "biomes");
		for (size_t k = 0; bj && k < bj->count && b->nbiomes < TRS_MAX_BIOMES; k++) {
			const trs_json *e = trs_json_at(bj, k);
			trs_biome *bi = &b->biomes[b->nbiomes++];
			snprintf(bi->id, sizeof bi->id, "%s", trs_json_str(e, "id", "?"));
			snprintf(bi->name, sizeof bi->name, "%s", trs_json_str(e, "name", bi->id));
			bi->abs_lat_min = trs_json_num(e, "abs_lat_min_deg", 0) * DEG;
			bi->abs_lat_max = trs_json_num(e, "abs_lat_max_deg", 90) * DEG;
			bi->height_min = trs_json_num(e, "height_min", -INFINITY);
			bi->height_max = trs_json_num(e, "height_max", INFINITY);
			const trs_json *n = trs_json_get(e, "near");
			if (n) {
				double la = trs_json_num(n, "lat_deg", 0) * DEG, lo = trs_json_num(n, "lon_deg", 0) * DEG;
				bi->has_near = 1;
				bi->near_dir = v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la));
				bi->near_radius = trs_json_num(n, "radius_km", 10) * 1000.0;
			}
		}
		const trs_json *lj = trs_json_get(j, "life");
		for (size_t k = 0; lj && k < lj->count && b->nlife < TRS_MAX_LIFE; k++) {
			const trs_json *e = trs_json_at(lj, k);
			trs_life *L = &b->life[b->nlife++];
			snprintf(L->id, sizeof L->id, "%s", trs_json_str(e, "id", "?"));
			L->fauna = trs_json_bool(e, "animal", 0);
			L->density = trs_json_num(e, "per_km2", 0);
			L->min_up = cos(trs_json_num(e, "max_slope_deg", 30) * DEG);
			L->height_min = trs_json_num(e, "height_min", -INFINITY);
			L->height_max = trs_json_num(e, "height_max", INFINITY);
			L->herd = (int)trs_json_num(e, "herd", 1);
			L->spread = trs_json_num(e, "spread", 10);
			L->loop = trs_json_num(e, "loop", 0);
			L->fly = trs_json_num(e, "fly", 0);
			const trs_json *bl = trs_json_get(e, "biomes");
			for (size_t q = 0; bl && q < bl->count; q++) {
				const trs_json *s = trs_json_at(bl, q);
				for (int bi = 0; bi < b->nbiomes; bi++)
					if (s && s->type == TRS_JSON_STRING && !strcmp(s->str, b->biomes[bi].id))
						L->biomes |= 1u << bi;
			}
		}
		const trs_json *rj = trs_json_get(j, "relics");
		for (size_t k = 0; rj && k < rj->count && b->nrelics < TRS_MAX_RELICS; k++) {
			const trs_json *e = trs_json_at(rj, k);
			trs_relic *r = &b->relics[b->nrelics++];
			snprintf(r->id, sizeof r->id, "%s", trs_json_str(e, "id", "?"));
			snprintf(r->name, sizeof r->name, "%s", trs_json_str(e, "name", r->id));
			double la = trs_json_num(e, "lat_deg", 0) * DEG, lo = trs_json_num(e, "lon_deg", 0) * DEG;
			r->dir = v3(cos(la) * cos(lo), cos(la) * sin(lo), sin(la));
		}
		const trs_json *ls = trs_json_get(j, "launch_site");
		if (ls) {
			b->has_site = 1;
			snprintf(b->site_name, sizeof b->site_name, "%s", trs_json_str(ls, "name", "Launch site"));
			b->site_lat = trs_json_num(ls, "lat_deg", 0) * DEG;
			b->site_lon = trs_json_num(ls, "lon_deg", 0) * DEG;
			b->site_alt = trs_json_num(ls, "altitude", 0);
			trs_terrain *t = &b->terrain;
			t->has_site = 1;
			t->site_dir = v3(cos(b->site_lat) * cos(b->site_lon), cos(b->site_lat) * sin(b->site_lon), sin(b->site_lat));
			t->site_radius = trs_json_num(ls, "flat_radius", 1500);
			t->site_height = b->site_alt;
		}
		b->soi = trs_json_num(j, "heliopause", INFINITY);
		if (b->parent >= 0) {
			trs_body *p = &sys->bodies[b->parent];
			const trs_json *o = trs_json_get(j, "orbit");
			if (!o) {
				snprintf(err, errlen, "body %s: orbit required", b->id);
				return 0;
			}
			double a = trs_json_num(o, "a", 0);
			trs_orbit_from_elements(&b->orbit, p->mu, a, trs_json_num(o, "e", 0), trs_json_num(o, "inc_deg", 0) * DEG,
					trs_json_num(o, "lan_deg", 0) * DEG, trs_json_num(o, "argp_deg", 0) * DEG,
					trs_json_num(o, "mean_anomaly_deg", 0) * DEG, 0.0);
			if (trs_json_bool(j, "tidally_locked", 0))
				b->rotation_period = b->orbit.period;
			b->soi = trs_json_num(j, "soi", a * pow(b->mu / p->mu, 0.4)); /* Laplace sphere */
			if (p->nchildren < TRS_MAX_BODIES)
				p->children[p->nchildren++] = sys->count;
		}
		sys->count++;
	}
	return 1;
}

static void start(trs_system *sys, const trs_json *root) {
	memset(sys, 0, sizeof(*sys));
	snprintf(sys->name, sizeof sys->name, "%s", trs_json_str(root, "name", "unnamed"));
}

int trs_system_load_json(trs_system *sys, const char *text, char *err, size_t errlen) {
	trs_json *root = trs_json_parse(text, err, errlen);
	if (!root)
		return 0;
	start(sys, root);
	int ok = add_bodies(sys, root, err, errlen);
	trs_json_free(root);
	return ok;
}

int trs_system_load(trs_system *sys, const char *path, char *err, size_t errlen) {
	trs_json *root = trs_json_load_file(path, err, errlen);
	if (!root)
		return 0;
	start(sys, root);
	int ok = add_bodies(sys, root, err, errlen);
	const trs_json *nb = trs_json_get(root, "neighbours");
	for (size_t k = 0; ok && nb && k < nb->count; k++) {
		const trs_json *e = trs_json_at(nb, k);
		char p2[1024];
		const char *slash = strrchr(path, '/');
		int dir = slash ? (int)(slash - path + 1) : 0;
		snprintf(p2, sizeof p2, "%.*s%s", dir, path, e && e->type == TRS_JSON_STRING ? e->str : "");
		trs_json *r2 = trs_json_load_file(p2, err, errlen);
		if (!r2) {
			ok = 0;
			break;
		}
		ok = add_bodies(sys, r2, err, errlen);
		trs_json_free(r2);
	}
	trs_json_free(root);
	return ok;
}

void trs_body_state(const trs_system *sys, int i, double t, trs_v3 *r, trs_v3 *v) {
	if (sys->bodies[i].parent < 0) {
		*r = v3(0, 0, 0);
		*v = v3(0, 0, 0);
		return;
	}
	trs_orbit_state_at(&sys->bodies[i].orbit, t, r, v);
}

void trs_body_state_abs(const trs_system *sys, int i, double t, trs_v3 *r, trs_v3 *v) {
	*r = v3(0, 0, 0);
	*v = v3(0, 0, 0);
	for (int k = i; k >= 0 && sys->bodies[k].parent >= 0; k = sys->bodies[k].parent) {
		trs_v3 rr, vv;
		trs_body_state(sys, k, t, &rr, &vv);
		*r = v3_add(*r, rr);
		*v = v3_add(*v, vv);
	}
}

void trs_body_state_galactic(const trs_system *sys, int i, double t, trs_v3 *r, trs_v3 *v) {
	trs_body_state_abs(sys, i, t, r, v);
	*r = v3_add(*r, sys->bodies[i].galactic);
}

void trs_body_state_from(const trs_system *sys, int i, int star, double t, trs_v3 *r, trs_v3 *v) {
	trs_body_state_abs(sys, i, t, r, v);
	if (sys->bodies[i].star != star) /* the stars' offset; never added within one system */
		*r = v3_add(*r, v3_sub(sys->bodies[i].galactic, sys->bodies[star].galactic));
}

double trs_body_rotation(const trs_body *b, double t) {
	if (b->rotation_period == 0)
		return b->rotation_at_epoch;
	return trs_wrap_tau(b->rotation_at_epoch + TRS_TAU * fmod(t / b->rotation_period, 1.0));
}

trs_v3 trs_body_omega(const trs_body *b) {
	return v3(0, 0, b->rotation_period != 0 ? TRS_TAU / b->rotation_period : 0);
}

void trs_atmosphere_at(const trs_body *b, double altitude, double *pressure, double *density) {
	if (b->atm.height <= 0 || altitude >= b->atm.height) {
		*pressure = 0;
		*density = 0;
		return;
	}
	if (altitude < 0)
		altitude = 0;
	/* Exponential profile, tapered linearly to exactly zero at the top so
	 * there is a crisp boundary where time warp becomes legal. */
	double k = exp(-altitude / b->atm.scale_height);
	double top = exp(-b->atm.height / b->atm.scale_height);
	double f = (k - top) / (1 - top);
	*pressure = b->atm.p0 * f;
	*density = b->atm.rho0 * f;
}

trs_v3 trs_body_to_fixed(const trs_body *b, double t, trs_v3 v) {
	double a = -trs_body_rotation(b, t), c = cos(a), s = sin(a);
	return v3(c * v.x - s * v.y, s * v.x + c * v.y, v.z);
}

trs_v3 trs_body_from_fixed(const trs_body *b, double t, trs_v3 v) {
	double a = trs_body_rotation(b, t), c = cos(a), s = sin(a);
	return v3(c * v.x - s * v.y, s * v.x + c * v.y, v.z);
}

double trs_body_terrain_at(const trs_body *b, double t, trs_v3 r) {
	if (!b->terrain.enabled)
		return 0;
	return trs_terrain_height(&b->terrain, b->radius, v3_norm(trs_body_to_fixed(b, t, r)));
}

trs_v3 trs_relic_position(const trs_body *b, int k) {
	trs_v3 d = b->relics[k].dir;
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, d) : 0.0;
	return v3_scale(d, b->radius + h);
}

trs_relic_view trs_relic_from(const trs_body *b, int k, trs_v3 p) {
	trs_relic_view out;
	trs_v3 s = trs_relic_position(b, k), d = b->relics[k].dir, u = v3_norm(p);
	out.distance = v3_len(v3_sub(p, s));
	out.surface_distance = atan2(v3_len(v3_cross(u, d)), v3_dot(u, d)) * b->radius;
	/* heading: the direction to it in the point's local east/north plane */
	trs_v3 z = v3(0, 0, 1);
	trs_v3 east = v3_cross(z, u);
	if (v3_len2(east) < 1e-24)
		east = v3(0, 1, 0); /* at a pole every way is south (or north) */
	east = v3_norm(east);
	trs_v3 north = v3_cross(u, east);
	out.bearing = atan2(v3_dot(d, east), v3_dot(d, north));
	if (out.bearing < 0)
		out.bearing += TRS_TAU;
	/* in sight: the point is above the relic's horizon, and the straight
	 * line between them clears the round body (terrain may still be in
	 * the way; the signal is strong enough not to mind) */
	trs_v3 ps = v3_sub(s, p);
	double L2 = v3_len2(ps), tt = L2 > 0 ? fmax(0.0, fmin(1.0, -v3_dot(p, ps) / L2)) : 0;
	double clear = fmin(b->radius, v3_len(s)) * (1 - 1e-9);
	out.in_sight = v3_dot(v3_sub(p, s), d) > 0 && (tt <= 0 || tt >= 1 || v3_len(v3_madd(p, ps, tt)) >= clear);
	return out;
}

int trs_body_biome(const trs_body *b, trs_v3 dir) {
	if (b->nbiomes == 0)
		return -1;
	double lat = fabs(asin(fmax(-1.0, fmin(1.0, dir.z))));
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, dir) : 0.0;
	for (int i = 0; i < b->nbiomes; i++) {
		const trs_biome *bi = &b->biomes[i];
		if (lat < bi->abs_lat_min || lat > bi->abs_lat_max || h < bi->height_min || h > bi->height_max)
			continue;
		if (bi->has_near && acos(fmax(-1.0, fmin(1.0, v3_dot(dir, bi->near_dir)))) * b->radius > bi->near_radius)
			continue;
		return i;
	}
	return b->nbiomes - 1;
}
