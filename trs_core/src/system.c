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

static int load_from(trs_system *sys, const trs_json *root, char *err, size_t errlen) {
	memset(sys, 0, sizeof(*sys));
	snprintf(sys->name, sizeof sys->name, "%s", trs_json_str(root, "name", "unnamed"));
	const trs_json *bodies = trs_json_get(root, "bodies");
	if (!bodies || bodies->type != TRS_JSON_ARRAY || bodies->count == 0) {
		snprintf(err, errlen, "system has no bodies");
		return 0;
	}
	if (bodies->count > TRS_MAX_BODIES) {
		snprintf(err, errlen, "too many bodies");
		return 0;
	}
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
		if (b->parent < 0 && sys->count > 0) {
			snprintf(err, errlen, "body %s: only the first body may be parentless", b->id);
			return 0;
		}
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
		b->soi = INFINITY;
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

int trs_system_load_json(trs_system *sys, const char *text, char *err, size_t errlen) {
	trs_json *root = trs_json_parse(text, err, errlen);
	if (!root)
		return 0;
	int ok = load_from(sys, root, err, errlen);
	trs_json_free(root);
	return ok;
}

int trs_system_load(trs_system *sys, const char *path, char *err, size_t errlen) {
	trs_json *root = trs_json_load_file(path, err, errlen);
	if (!root)
		return 0;
	int ok = load_from(sys, root, err, errlen);
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
