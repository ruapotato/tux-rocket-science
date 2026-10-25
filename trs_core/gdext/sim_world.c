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

/* TrsSim world API: bodies, vessels, modes, warp. Positions come out as
 * PackedFloat64Array so views keep full double precision until the floating
 * origin subtraction; structured data comes out as JSON. */
#include "sim.h"
#include "trs/json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG (180.0 / TRS_PI)

static trs_sim_handle *H(void *self) { return self; }

static int vessel_ok(trs_sim_handle *h, int64_t id) {
	return h->world && id >= 0 && id < h->world->nvessels && h->world->vessels[id].alive;
}

static void ret_arr(trs_val *r, const double *d, int n) {
	r->arr.n = n;
	r->arr.data = malloc(sizeof(double) * (size_t)(n ? n : 1));
	memcpy(r->arr.data, d, sizeof(double) * (size_t)n);
}

static void ret_json(trs_val *r, trs_json *j) {
	r->s = trs_json_write_compact(j);
	trs_json_free(j);
}

/* load_system(path) -> "" on success, else the error message */
static void m_load_system(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_system sys;
	if (!trs_system_load(&sys, a[0].s, h->err, sizeof h->err)) {
		r->s = trs_strdup_printf("%s", h->err);
		return;
	}
	if (!h->world)
		h->world = malloc(sizeof(trs_world));
	trs_world_init(h->world, &sys, 0);
	r->s = trs_strdup_printf("%s", "");
}

/* world_save() -> the whole world as JSON text ("" if there is none) */
static void m_world_save(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!h->world) {
		r->s = trs_strdup_printf("%s", "");
		return;
	}
	trs_json *j = trs_world_to_json(h->world);
	r->s = trs_json_write_compact(j);
	trs_json_free(j);
}

/* world_load(text) -> true, or false with last_error() set (world untouched) */
static void m_world_load(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->b = 0;
	if (!h->world || !h->catalog_loaded) {
		snprintf(h->err, sizeof h->err, "load a system and a part catalog first");
		return;
	}
	trs_json *j = trs_json_parse(a[0].s, h->err, sizeof h->err);
	if (!j)
		return;
	r->b = trs_world_from_json(h->world, &h->catalog, j, h->err, sizeof h->err);
	trs_json_free(j);
}

static void m_reset_time(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (h->world) {
		h->world->t = a[0].f;
		h->world->t_comp = 0;
	}
}

static void m_time(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->f = h->world ? h->world->t : 0;
}

static void m_body_count(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = h->world ? h->world->sys.count : 0;
}

static void m_body_find(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = h->world ? trs_system_find(&h->world->sys, a[0].s) : -1;
}

/* body_info(i) -> JSON */
static void m_body_info(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!h->world || a[0].i < 0 || a[0].i >= h->world->sys.count) {
		r->s = trs_strdup_printf("{}");
		return;
	}
	const trs_system *sys = &h->world->sys;
	const trs_body *b = &sys->bodies[a[0].i];
	trs_json *j = trs_json_new(TRS_JSON_OBJECT);
	trs_json_set(j, "id", trs_json_new_str(b->id));
	trs_json_set(j, "name", trs_json_new_str(b->name));
	trs_json_set(j, "index", trs_json_new_num((double)a[0].i));
	trs_json_set(j, "parent", trs_json_new_num(b->parent));
	trs_json_set(j, "mu", trs_json_new_num(b->mu));
	trs_json_set(j, "radius", trs_json_new_num(b->radius));
	trs_json_set(j, "soi", trs_json_new_num(isfinite(b->soi) ? b->soi : -1));
	trs_json_set(j, "rotation_period", trs_json_new_num(b->rotation_period));
	trs_json_set(j, "atmosphere_height", trs_json_new_num(b->atm.height));
	trs_json_set(j, "scale_height", trs_json_new_num(b->atm.scale_height));
	trs_json_set(j, "surface_gravity", trs_json_new_num(b->mu / (b->radius * b->radius)));
	if (b->has_site) {
		trs_json *site = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(site, "name", trs_json_new_str(b->site_name));
		trs_json_set(site, "lat_deg", trs_json_new_num(b->site_lat * DEG));
		trs_json_set(site, "lon_deg", trs_json_new_num(b->site_lon * DEG));
		trs_json_set(site, "altitude", trs_json_new_num(b->site_alt));
		trs_json_set(j, "site", site);
	}
	if (b->parent >= 0) {
		trs_json_set(j, "sma", trs_json_new_num(b->orbit.a));
		trs_json_set(j, "period", trs_json_new_num(b->orbit.period));
	}
	ret_json(r, j);
}

/* body_state(i) -> [rx,ry,rz, vx,vy,vz, rotation] relative to the root star */
static void m_body_state(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double d[7] = { 0 };
	if (h->world && a[0].i >= 0 && a[0].i < h->world->sys.count) {
		trs_v3 rr, vv;
		trs_body_state_abs(&h->world->sys, (int)a[0].i, h->world->t, &rr, &vv);
		d[0] = rr.x, d[1] = rr.y, d[2] = rr.z, d[3] = vv.x, d[4] = vv.y, d[5] = vv.z;
		d[6] = trs_body_rotation(&h->world->sys.bodies[a[0].i], h->world->t);
	}
	ret_arr(r, d, 7);
}

/* vessel_add(body, [rx,ry,rz,vx,vy,vz]) -> id (physics mode) */
static void m_vessel_add(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = -1;
	if (!h->world || a[1].arr.n < 6 || a[0].i < 0 || a[0].i >= h->world->sys.count)
		return;
	const double *d = a[1].arr.data;
	r->i = trs_world_add_vessel(h->world, (int)a[0].i, v3(d[0], d[1], d[2]), v3(d[3], d[4], d[5]));
}

static void m_vessel_remove(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (vessel_ok(h, a[0].i))
		trs_world_remove_vessel(h->world, (int)a[0].i);
}

/* vessel_state(id) -> [body, rx,ry,rz, vx,vy,vz, mode] relative to its SOI body */
static void m_vessel_state(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double d[8] = { -1, 0, 0, 0, 0, 0, 0, 0 };
	if (vessel_ok(h, a[0].i)) {
		trs_v3 rr, vv;
		trs_vessel_state(h->world, (int)a[0].i, &rr, &vv);
		const trs_vessel *vs = &h->world->vessels[a[0].i];
		d[0] = vs->body;
		d[1] = rr.x, d[2] = rr.y, d[3] = rr.z, d[4] = vv.x, d[5] = vv.y, d[6] = vv.z;
		d[7] = vs->mode;
	}
	ret_arr(r, d, 8);
}

static trs_json *orbit_json(const trs_system *sys, int body, const trs_orbit *o, double t) {
	const trs_body *b = &sys->bodies[body];
	trs_json *j = trs_json_new(TRS_JSON_OBJECT);
	int closed = trs_orbit_is_closed(o);
	trs_json_set(j, "body", trs_json_new_str(b->id));
	trs_json_set(j, "closed", trs_json_new_bool(closed));
	trs_json_set(j, "sma", trs_json_new_num(o->a));
	trs_json_set(j, "ecc", trs_json_new_num(o->e));
	trs_json_set(j, "inc_deg", trs_json_new_num(o->inc * DEG));
	trs_json_set(j, "lan_deg", trs_json_new_num(o->lan * DEG));
	trs_json_set(j, "argp_deg", trs_json_new_num(o->argp * DEG));
	trs_json_set(j, "true_anomaly_deg", trs_json_new_num(trs_orbit_true_anomaly_at(o, t) * DEG));
	trs_json_set(j, "period", trs_json_new_num(o->period));
	trs_json_set(j, "energy", trs_json_new_num(o->energy));
	trs_json_set(j, "pe_alt", trs_json_new_num(o->rp - b->radius));
	trs_json_set(j, "ap_alt", closed ? trs_json_new_num(o->ra - b->radius) : trs_json_new(TRS_JSON_NULL));
	trs_json_set(j, "t_to_pe", trs_json_new_num(trs_orbit_next_periapsis(o, t) - t));
	double tap = trs_orbit_next_apoapsis(o, t);
	trs_json_set(j, "t_to_ap", isfinite(tap) ? trs_json_new_num(tap - t) : trs_json_new(TRS_JSON_NULL));
	return j;
}

/* vessel_orbit(id) -> JSON osculating elements and apsides */
static void m_vessel_orbit(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!vessel_ok(h, a[0].i)) {
		r->s = trs_strdup_printf("{}");
		return;
	}
	trs_orbit o;
	trs_vessel_orbit(h->world, (int)a[0].i, &o);
	trs_json *j = orbit_json(&h->world->sys, h->world->vessels[a[0].i].body, &o, h->world->t);
	trs_json_set(j, "mode", trs_json_new_str(h->world->vessels[a[0].i].mode == TRS_MODE_RAILS ? "rails" : "physics"));
	ret_json(r, j);
}

static void m_vessel_set_rails(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (vessel_ok(h, a[0].i))
		trs_vessel_set_mode(h->world, (int)a[0].i, a[1].b ? TRS_MODE_RAILS : TRS_MODE_PHYSICS);
}

/* step_physics(ticks) */
static void m_step_physics(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!h->world)
		return;
	for (int64_t i = 0; i < a[0].i; i++)
		trs_world_step_physics(h->world);
}

static const char *event_name(trs_event_kind k) {
	switch (k) {
	case TRS_EV_NONE: return "none";
	case TRS_EV_SOI_EXIT: return "soi_exit";
	case TRS_EV_SOI_ENTER: return "soi_enter";
	case TRS_EV_ATMOSPHERE: return "atmosphere";
	case TRS_EV_SURFACE: return "surface";
	}
	return "?";
}

/* warp(dt, stop_on_vessel) -> JSON {"event","t","body"}. Refused (event
 * "refused") unless every live vessel is on rails. */
static void m_warp(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *j = trs_json_new(TRS_JSON_OBJECT);
	if (!h->world) {
		trs_json_set(j, "event", trs_json_new_str("refused"));
		ret_json(r, j);
		return;
	}
	for (int i = 0; i < h->world->nvessels; i++)
		if (h->world->vessels[i].alive && h->world->vessels[i].mode != TRS_MODE_RAILS) {
			trs_json_set(j, "event", trs_json_new_str("refused"));
			trs_json_set(j, "reason", trs_json_new_str("vessel in physics mode"));
			ret_json(r, j);
			return;
		}
	trs_event ev;
	int stop = vessel_ok(h, a[1].i) ? (int)a[1].i : -1;
	trs_world_warp(h->world, a[0].f, stop, &ev);
	trs_json_set(j, "event", trs_json_new_str(event_name(ev.kind)));
	trs_json_set(j, "t", trs_json_new_num(h->world->t));
	if (ev.kind != TRS_EV_NONE)
		trs_json_set(j, "body", trs_json_new_str(h->world->sys.bodies[ev.body].id));
	ret_json(r, j);
}

/* predict(id, max_patches, horizon_s) -> JSON [{orbit..., t_start, t_end, end}] */
static void m_predict(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	if (vessel_ok(h, a[0].i)) {
		const trs_system *sys = &h->world->sys;
		int body = h->world->vessels[a[0].i].body;
		trs_orbit o;
		trs_vessel_orbit(h->world, (int)a[0].i, &o);
		double t = h->world->t, t_max = t + a[2].f;
		for (int64_t k = 0; k < a[1].i; k++) {
			trs_event ev = trs_next_event(sys, body, &o, t, t_max);
			trs_json *p = orbit_json(sys, body, &o, t);
			trs_json_set(p, "t_start", trs_json_new_num(t));
			trs_json_set(p, "t_end", trs_json_new_num(ev.kind == TRS_EV_NONE ? t_max : ev.t));
			trs_json_set(p, "end", trs_json_new_str(event_name(ev.kind)));
			trs_json_push(arr, p);
			if (ev.kind != TRS_EV_SOI_ENTER && ev.kind != TRS_EV_SOI_EXIT)
				break;
			trs_v3 rr, vv;
			trs_orbit_state_at(&o, ev.t, &rr, &vv);
			trs_rebase_state(sys, body, ev.body, ev.t, &rr, &vv);
			body = ev.body;
			trs_orbit_from_state(&o, sys->bodies[body].mu, rr, vv, ev.t);
			t = ev.t;
		}
	}
	ret_json(r, arr);
}

#define S TRS_T_STRING
#define I TRS_T_INT
#define F TRS_T_FLOAT
#define A TRS_T_F64ARR
#define B TRS_T_BOOL
#define V TRS_T_VOID
const trs_method_def TRS_SIM_WORLD_METHODS[] = {
	{ "load_system", m_load_system, S, 1, { S }, { "path" } },
	{ "reset_time", m_reset_time, V, 1, { F }, { "t" } },
	{ "world_save", m_world_save, S, 0, { 0 }, { 0 } },
	{ "world_load", m_world_load, B, 1, { S }, { "text" } },
	{ "time", m_time, F, 0, { 0 }, { 0 } },
	{ "body_count", m_body_count, I, 0, { 0 }, { 0 } },
	{ "body_find", m_body_find, I, 1, { S }, { "id" } },
	{ "body_info", m_body_info, S, 1, { I }, { "index" } },
	{ "body_state", m_body_state, A, 1, { I }, { "index" } },
	{ "vessel_add", m_vessel_add, I, 2, { I, A }, { "body", "state" } },
	{ "vessel_remove", m_vessel_remove, V, 1, { I }, { "id" } },
	{ "vessel_state", m_vessel_state, A, 1, { I }, { "id" } },
	{ "vessel_orbit", m_vessel_orbit, S, 1, { I }, { "id" } },
	{ "vessel_set_rails", m_vessel_set_rails, V, 2, { I, B }, { "id", "rails" } },
	{ "step_physics", m_step_physics, V, 1, { I }, { "ticks" } },
	{ "warp", m_warp, S, 2, { F, I }, { "dt", "stop_on" } },
	{ "predict", m_predict, S, 3, { I, I, F }, { "id", "max_patches", "horizon" } },
};
const int TRS_SIM_WORLD_METHOD_COUNT = sizeof(TRS_SIM_WORLD_METHODS) / sizeof(TRS_SIM_WORLD_METHODS[0]);
