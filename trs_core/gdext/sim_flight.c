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

/* TrsSim flight API: launch, controls, telemetry, render feeds, terrain. */
#include "sim.h"
#include "trs/craft.h"
#include "trs/json.h"
#include "trs/terrain.h"
#include "trs/trajectory.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define DEG (180.0 / TRS_PI)

static trs_sim_handle *H(void *self) { return self; }

static int ok_vessel(trs_sim_handle *h, int64_t id) {
	return h->world && id >= 0 && id < h->world->nvessels && h->world->vessels[id].alive;
}

static int ok_body(trs_sim_handle *h, int64_t b) {
	return h->world && b >= 0 && b < h->world->sys.count;
}

static void ret_doubles(trs_val *r, const double *d, int64_t n) {
	r->arr.n = n;
	r->arr.data = malloc(sizeof(double) * (size_t)(n ? n : 1));
	if (n)
		memcpy(r->arr.data, d, sizeof(double) * (size_t)n);
}

static void ret_json(trs_val *r, trs_json *j) {
	r->s = trs_json_write_compact(j);
	trs_json_free(j);
}

static trs_json *v3_json(trs_v3 v) {
	trs_json *a = trs_json_new(TRS_JSON_ARRAY);
	trs_json_push(a, trs_json_new_num(v.x));
	trs_json_push(a, trs_json_new_num(v.y));
	trs_json_push(a, trs_json_new_num(v.z));
	return a;
}

static int craft_from_text(trs_sim_handle *h, const char *text, trs_craft *c) {
	if (!h->catalog_loaded) {
		snprintf(h->err, sizeof h->err, "no part catalog loaded");
		return 0;
	}
	trs_json *j = trs_json_parse(text, h->err, sizeof h->err);
	if (!j)
		return 0;
	int ok = trs_craft_from_json(c, &h->catalog, j, h->err, sizeof h->err);
	trs_json_free(j);
	return ok;
}

/* flight_launch(craft_text, body_id) -> vessel id at the body's launch site */
static void m_flight_launch(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = -1;
	trs_craft c;
	if (!h->world || !craft_from_text(h, a[0].s, &c))
		return;
	int b = trs_system_find(&h->world->sys, a[1].s);
	if (b < 0 || !h->world->sys.bodies[b].has_site) {
		snprintf(h->err, sizeof h->err, "no launch site on %s", a[1].s);
		trs_craft_free(&c);
		return;
	}
	const trs_body *bd = &h->world->sys.bodies[b];
	r->i = trs_world_launch(h->world, &c, b, bd->site_lat, bd->site_lon);
	trs_craft_free(&c);
}

/* flight_spawn_orbit(craft_text, body_id, altitude, inc_deg) -> id, circular */
static void m_flight_spawn_orbit(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = -1;
	trs_craft c;
	if (!h->world || !craft_from_text(h, a[0].s, &c))
		return;
	int b = trs_system_find(&h->world->sys, a[1].s);
	if (b < 0) {
		trs_craft_free(&c);
		return;
	}
	const trs_body *bd = &h->world->sys.bodies[b];
	double rr = bd->radius + a[2].f, vv = sqrt(bd->mu / rr), inc = a[3].f / DEG;
	trs_v3 pos = v3(rr, 0, 0), vel = v3(0, vv * cos(inc), vv * sin(inc));
	trs_quat q = quat_from_to(v3(0, 1, 0), v3_norm(vel));
	r->i = trs_world_add_craft(h->world, &c, b, pos, vel, q);
	trs_craft_free(&c);
}

static void m_vessel_set_throttle(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (ok_vessel(h, a[0].i))
		h->world->vessels[a[0].i].throttle = fmax(0, fmin(1, a[1].f));
}

static void m_vessel_set_input(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (ok_vessel(h, a[0].i))
		h->world->vessels[a[0].i].input = v3(fmax(-1, fmin(1, a[1].f)), fmax(-1, fmin(1, a[2].f)), fmax(-1, fmin(1, a[3].f)));
}

static const char *SAS_NAMES[TRS_SAS_COUNT] = { "off", "stability", "target", "prograde", "retrograde", "normal",
	"antinormal", "radial_out", "radial_in", "surface_prograde", "surface_retrograde" };

/* vessel_set_sas(id, mode, target[3]) */
static void m_vessel_set_sas(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->b = 0;
	if (!ok_vessel(h, a[0].i))
		return;
	trs_vessel *vs = &h->world->vessels[a[0].i];
	for (int m = 0; m < TRS_SAS_COUNT; m++)
		if (!strcmp(SAS_NAMES[m], a[1].s)) {
			if (m == TRS_SAS_STABILITY && vs->sas != TRS_SAS_STABILITY) {
				trs_v3 rr, vv;
				trs_vessel_state(h->world, (int)a[0].i, &rr, &vv);
				vs->sas_hold = vs->mode == TRS_MODE_LANDED
						? quat_mul(trs_body_quat(&h->world->sys.bodies[vs->body], h->world->t), vs->fixed_q)
						: vs->q;
			}
			vs->sas = (trs_sas_mode)m;
			if (m == TRS_SAS_TARGET && a[2].arr.n >= 3)
				vs->sas_target = v3_norm(v3(a[2].arr.data[0], a[2].arr.data[1], a[2].arr.data[2]));
			r->b = 1;
			return;
		}
}

/* vessel_stage(id) -> JSON array of new vessel ids */
static void m_vessel_stage(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	if (ok_vessel(h, a[0].i)) {
		int ids[32];
		int n = trs_world_stage(h->world, (int)a[0].i, ids, 32);
		for (int k = 0; k < n; k++)
			trs_json_push(arr, trs_json_new_num(ids[k]));
	}
	ret_json(r, arr);
}

static void m_vessel_set_legs(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_vessel(h, a[0].i) || !h->world->vessels[a[0].i].craft)
		return;
	trs_craft *c = h->world->vessels[a[0].i].craft;
	for (int i = 0; i < c->count; i++)
		if (trs_craft_def(c, i)->is_leg)
			c->parts[i].legs_deployed = a[1].b;
	trs_vessel_refresh(h->world, (int)a[0].i);
}

/* vessel_deploy_chutes(id) -> how many chutes were armed */
static void m_vessel_deploy_chutes(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = ok_vessel(h, a[0].i) ? trs_vessel_deploy_chutes(h->world, (int)a[0].i) : 0;
}

static const char *MODE_NAMES[] = { "physics", "rails", "landed" };

/* vessel_set_mode(id, "physics"|"rails"|"landed") */
static void m_vessel_set_mode(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_vessel(h, a[0].i))
		return;
	for (int m = 0; m < 3; m++)
		if (!strcmp(MODE_NAMES[m], a[1].s))
			trs_vessel_set_mode(h->world, (int)a[0].i, (trs_mode)m);
}

static void m_vessel_alive(void *self, const trs_val *a, trs_val *r) {
	r->b = ok_vessel(H(self), a[0].i);
}

/* Local frame at a position: up, north, east (SOI frame). */
static void local_frame(trs_v3 r, trs_v3 *up, trs_v3 *north, trs_v3 *east) {
	*up = v3_norm(r);
	trs_v3 e = v3_cross(v3(0, 0, 1), *up);
	if (v3_len2(e) < 1e-20)
		e = v3(0, 1, 0);
	*east = v3_norm(e);
	*north = v3_cross(*up, *east);
}

static trs_quat vessel_quat(const trs_world *w, const trs_vessel *vs) {
	if (vs->mode == TRS_MODE_LANDED)
		return quat_normalize(quat_mul(trs_body_quat(&w->sys.bodies[vs->body], w->t), vs->fixed_q));
	return vs->q;
}

static const char *situation(const trs_world *w, int id, const trs_orbit *o) {
	const trs_vessel *vs = &w->vessels[id];
	const trs_body *b = &w->sys.bodies[vs->body];
	if (vs->clamped)
		return "prelaunch";
	if (vs->landed || vs->mode == TRS_MODE_LANDED)
		return "landed";
	trs_v3 r, v;
	trs_vessel_state(w, id, &r, &v);
	double alt = v3_len(r) - b->radius;
	if (b->atm.height > 0 && alt < b->atm.height)
		return "flying";
	double floor_r = b->radius + (b->atm.height > 0 ? b->atm.height : 0);
	if (!trs_orbit_is_closed(o))
		return "escaping";
	if (o->rp < floor_r)
		return "suborbital";
	if (b->parent >= 0 && o->ra > b->soi)
		return "escaping";
	return "orbiting";
}

/* telemetry(id) -> JSON: everything the HUD and the autopilots read. */
static double heat_share(const trs_craft *c, int i) {
	const trs_part_def *d = trs_craft_def(c, i);
	return (c->parts[i].temp - TRS_T_AMBIENT) / (d->max_temp - TRS_T_AMBIENT);
}

static void m_telemetry(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *j = trs_json_new(TRS_JSON_OBJECT);
	if (!ok_vessel(h, a[0].i)) {
		trs_json_set(j, "alive", trs_json_new_bool(0));
		ret_json(r, j);
		return;
	}
	trs_world *w = h->world;
	int id = (int)a[0].i;
	trs_vessel *vs = &w->vessels[id];
	const trs_body *b = &w->sys.bodies[vs->body];
	trs_v3 pos, vel;
	trs_vessel_state(w, id, &pos, &vel);
	trs_orbit o;
	trs_vessel_orbit(w, id, &o);
	double rl = v3_len(pos);
	double terrain = trs_body_terrain_at(b, w->t, pos);
	trs_v3 vsurf = v3_sub(vel, v3_cross(trs_body_omega(b), pos));
	trs_v3 up, north, east;
	local_frame(pos, &up, &north, &east);
	trs_quat q = vessel_quat(w, vs);
	trs_v3 nose = quat_rotate(q, v3(0, 1, 0)), side = quat_rotate(q, v3(1, 0, 0));
	double pitch = asin(fmax(-1, fmin(1, v3_dot(nose, up))));
	double heading = atan2(v3_dot(nose, east), v3_dot(nose, north));
	if (heading < 0)
		heading += TRS_TAU;
	/* roll: angle of the vessel's +X about the nose, from the horizontal */
	trs_v3 horiz = v3_norm(v3_cross(nose, up));
	double roll = v3_len2(horiz) > 0 ? atan2(v3_dot(side, v3_cross(horiz, nose)), v3_dot(side, horiz)) : 0;
	double p, rho;
	trs_atmosphere_at(b, rl - b->radius, &p, &rho);
	trs_v3 bfix = trs_body_to_fixed(b, w->t, pos);
	trs_v3 bdir = v3_norm(bfix);

	trs_json_set(j, "alive", trs_json_new_bool(1));
	trs_json_set(j, "id", trs_json_new_num(id));
	trs_json_set(j, "name", trs_json_new_str(vs->craft ? vs->craft->name : "point"));
	trs_json_set(j, "t", trs_json_new_num(w->t));
	trs_json_set(j, "body", trs_json_new_str(b->id));
	trs_json_set(j, "body_index", trs_json_new_num(vs->body));
	trs_json_set(j, "mode", trs_json_new_str(MODE_NAMES[vs->mode]));
	trs_json_set(j, "situation", trs_json_new_str(situation(w, id, &o)));
	trs_json_set(j, "altitude", trs_json_new_num(rl - b->radius));
	trs_json_set(j, "altitude_terrain", trs_json_new_num(rl - b->radius - terrain));
	trs_json_set(j, "terrain_height", trs_json_new_num(terrain));
	trs_json_set(j, "lat_deg", trs_json_new_num(asin(bdir.z) * DEG));
	trs_json_set(j, "lon_deg", trs_json_new_num(atan2(bdir.y, bdir.x) * DEG));
	trs_json_set(j, "orbital_speed", trs_json_new_num(v3_len(vel)));
	trs_json_set(j, "surface_speed", trs_json_new_num(v3_len(vsurf)));
	trs_json_set(j, "vertical_speed", trs_json_new_num(v3_dot(vel, up)));
	trs_json_set(j, "horizontal_speed", trs_json_new_num(v3_len(v3_sub(vsurf, v3_scale(up, v3_dot(vsurf, up))))));
	trs_json_set(j, "pitch_deg", trs_json_new_num(pitch * DEG));
	trs_json_set(j, "heading_deg", trs_json_new_num(heading * DEG));
	trs_json_set(j, "roll_deg", trs_json_new_num(roll * DEG));
	trs_json_set(j, "pressure", trs_json_new_num(p));
	trs_json_set(j, "density", trs_json_new_num(rho));
	trs_json_set(j, "dynamic_pressure", trs_json_new_num(0.5 * rho * v3_len2(vsurf)));
	trs_json_set(j, "g_force", trs_json_new_num(v3_len(vs->sensed_accel) / TRS_G0));
	trs_json_set(j, "atmosphere_height", trs_json_new_num(b->atm.height));
	trs_json_set(j, "in_atmosphere", trs_json_new_bool(b->atm.height > 0 && rl - b->radius < b->atm.height));
	trs_json_set(j, "landed", trs_json_new_bool(vs->landed || vs->mode == TRS_MODE_LANDED));
	trs_json_set(j, "clamped", trs_json_new_bool(vs->clamped));
	trs_json_set(j, "contacts", trs_json_new_num(vs->contacts));
	trs_json_set(j, "max_impact", trs_json_new_num(vs->max_impact));
	trs_json_set(j, "spin", trs_json_new_num(vs->mode == TRS_MODE_PHYSICS ? v3_len(v3_sub(vs->w, trs_body_omega(b))) : 0.0));
	trs_json_set(j, "throttle", trs_json_new_num(vs->throttle));
	trs_json_set(j, "dv_expended", trs_json_new_num(vs->dv_expended));
	trs_json_set(j, "sas", trs_json_new_str(SAS_NAMES[vs->sas]));
	trs_json_set(j, "next_stage", trs_json_new_num(vs->next_stage));
	trs_json_set(j, "up", v3_json(up));
	trs_json_set(j, "north", v3_json(north));
	trs_json_set(j, "east", v3_json(east));
	trs_json_set(j, "nose", v3_json(nose));
	trs_json_set(j, "side", v3_json(side));
	trs_json_set(j, "position", v3_json(pos));
	trs_json_set(j, "velocity", v3_json(vel));
	trs_json_set(j, "surface_velocity", v3_json(vsurf));

	/* orbit */
	trs_json *oj = trs_json_new(TRS_JSON_OBJECT);
	int closed = trs_orbit_is_closed(&o);
	trs_json_set(oj, "sma", trs_json_new_num(o.a));
	trs_json_set(oj, "ecc", trs_json_new_num(o.e));
	trs_json_set(oj, "inc_deg", trs_json_new_num(o.inc * DEG));
	trs_json_set(oj, "period", trs_json_new_num(o.period));
	trs_json_set(oj, "pe_alt", trs_json_new_num(o.rp - b->radius));
	trs_json_set(oj, "ap_alt", closed ? trs_json_new_num(o.ra - b->radius) : trs_json_new(TRS_JSON_NULL));
	trs_json_set(oj, "t_to_pe", trs_json_new_num(trs_orbit_next_periapsis(&o, w->t) - w->t));
	double tap = trs_orbit_next_apoapsis(&o, w->t);
	trs_json_set(oj, "t_to_ap", isfinite(tap) ? trs_json_new_num(tap - w->t) : trs_json_new(TRS_JSON_NULL));
	trs_json_set(j, "orbit", oj);

	/* craft: mass, thrust, and live per-stage readouts from the current state */
	if (vs->craft) {
		trs_craft *c = vs->craft;
		double g_local = b->mu / (rl * rl);
		double thrust = 0, thrust_max = 0;
		int lit = 0;
		for (int i = 0; i < c->count; i++) {
			const trs_part_def *d = trs_craft_def(c, i);
			if (!c->parts[i].engine_on || d->propellant == TRS_PROP_NONE || !trs_craft_engine_fed(c, vs->fuel_group, i))
				continue;
			lit++;
			double full = trs_engine_mdot(d) * trs_engine_isp(d, p) * TRS_G0;
			thrust_max += full;
			thrust += full * (d->propellant == TRS_PROP_SOLID ? 1.0 : vs->throttle);
		}
		trs_json_set(j, "mass", trs_json_new_num(vs->mass));
		trs_json_set(j, "parts", trs_json_new_num(c->count));
		trs_json_set(j, "thrust", trs_json_new_num(thrust));
		trs_json_set(j, "thrust_max", trs_json_new_num(thrust_max));
		trs_json_set(j, "engines_lit", trs_json_new_num(lit));
		trs_json_set(j, "twr", trs_json_new_num(thrust / (vs->mass * g_local)));
		trs_json_set(j, "twr_max", trs_json_new_num(thrust_max / (vs->mass * g_local)));
		int crew = 0;
		for (int i = 0; i < c->count; i++)
			crew += trs_craft_def(c, i)->crew;
		trs_json_set(j, "crew_capacity", trs_json_new_num(crew));
		trs_stage_stats st[32];
		int ns = trs_craft_stage_stats(c, vs->next_stage + 1, g_local, p, st, 32);
		trs_json *stages = trs_json_new(TRS_JSON_ARRAY);
		double dv_total = 0;
		for (int k = 0; k < ns; k++) {
			trs_json *sj = trs_json_new(TRS_JSON_OBJECT);
			trs_json_set(sj, "stage", trs_json_new_num(st[k].stage));
			trs_json_set(sj, "dv", trs_json_new_num(st[k].dv_sl));
			trs_json_set(sj, "dv_vac", trs_json_new_num(st[k].dv_vac));
			trs_json_set(sj, "twr", trs_json_new_num(st[k].twr_sl));
			trs_json_set(sj, "burn_time", trs_json_new_num(st[k].burn_time));
			trs_json_set(sj, "fuel", trs_json_new_num(st[k].mass_start - st[k].mass_end));
			trs_json_push(stages, sj);
			dv_total += st[k].dv_sl;
		}
		trs_json_set(j, "stages", stages);
		trs_json_set(j, "dv_total", trs_json_new_num(dv_total));
		/* what each remaining SPACE press does, next one first:
		 * [{stage, engines, decouplers, chutes}] */
		trs_json *plan = trs_json_new(TRS_JSON_ARRAY);
		for (int sg = vs->next_stage; sg >= 0; sg--) {
			int ne = 0, nd = 0, nc = 0;
			for (int i = 0; i < c->count; i++) {
				if (c->parts[i].stage != sg)
					continue;
				const trs_part_def *d = trs_craft_def(c, i);
				ne += d->propellant != TRS_PROP_NONE;
				nd += d->decoupler != 0 && !c->parts[i].fired;
				nc += d->is_parachute;
			}
			if (ne + nd + nc == 0)
				continue;
			trs_json *pj = trs_json_new(TRS_JSON_OBJECT);
			trs_json_set(pj, "stage", trs_json_new_num(sg));
			trs_json_set(pj, "engines", trs_json_new_num(ne));
			trs_json_set(pj, "decouplers", trs_json_new_num(nd));
			trs_json_set(pj, "chutes", trs_json_new_num(nc));
			trs_json_push(plan, pj);
		}
		trs_json_set(j, "stage_plan", plan);
		/* propellant remaining in the current fuel groups */
		double liquid = 0, solid = 0, liquid_cap = 0, solid_cap = 0;
		for (int i = 0; i < c->count; i++) {
			const trs_part_def *d = trs_craft_def(c, i);
			if (d->fuel_liquid > 0)
				liquid += c->parts[i].fuel, liquid_cap += d->fuel_liquid;
			if (d->fuel_solid > 0)
				solid += c->parts[i].fuel, solid_cap += d->fuel_solid;
		}
		trs_json_set(j, "liquid", trs_json_new_num(liquid));
		trs_json_set(j, "liquid_capacity", trs_json_new_num(liquid_cap));
		trs_json_set(j, "solid", trs_json_new_num(solid));
		trs_json_set(j, "solid_capacity", trs_json_new_num(solid_cap));
		/* the current stage: propellant feeding the lit engines (or, between
		 * stages, the engines the next stage will light) */
		if (vs->dirty)
			trs_vessel_refresh(w, id);
		unsigned char *grp = calloc((size_t)c->count + 1, 1);
		int any_lit = 0;
		for (int i = 0; i < c->count; i++)
			if (c->parts[i].engine_on && trs_craft_def(c, i)->propellant != TRS_PROP_NONE)
				any_lit = 1;
		for (int i = 0; i < c->count; i++) {
			const trs_part_def *d = trs_craft_def(c, i);
			if (d->propellant == TRS_PROP_NONE)
				continue;
			if (any_lit ? !c->parts[i].engine_on : c->parts[i].stage != vs->next_stage)
				continue;
			grp[i] = 1; /* solids carry their own propellant */
			if (d->propellant == TRS_PROP_LIQUID)
				for (int k = 0; k < c->count; k++)
					if (vs->fuel_group[k] == vs->fuel_group[i])
						grp[k] = 1;
		}
		double sl = 0, slc = 0, ss = 0, ssc = 0;
		for (int i = 0; i < c->count; i++) {
			if (!grp[i])
				continue;
			const trs_part_def *d = trs_craft_def(c, i);
			if (d->fuel_liquid > 0)
				sl += c->parts[i].fuel, slc += d->fuel_liquid;
			if (d->fuel_solid > 0)
				ss += c->parts[i].fuel, ssc += d->fuel_solid;
		}
		free(grp);
		trs_json_set(j, "stage_liquid", trs_json_new_num(sl));
		trs_json_set(j, "stage_liquid_capacity", trs_json_new_num(slc));
		trs_json_set(j, "stage_solid", trs_json_new_num(ss));
		trs_json_set(j, "stage_solid_capacity", trs_json_new_num(ssc));
		int chutes = 0, chutes_stowed = 0, chutes_open = 0;
		for (int i = 0; i < c->count; i++)
			if (trs_craft_def(c, i)->is_parachute) {
				chutes++;
				chutes_stowed += c->parts[i].chute == TRS_CHUTE_STOWED;
				chutes_open += c->parts[i].chute == TRS_CHUTE_SEMI || c->parts[i].chute == TRS_CHUTE_FULL;
			}
		trs_json_set(j, "chutes", trs_json_new_num(chutes));
		trs_json_set(j, "chutes_stowed", trs_json_new_num(chutes_stowed));
		trs_json_set(j, "chutes_open", trs_json_new_num(chutes_open));
		/* hottest part, as the share of its headroom used: 0 at rest
		 * (ambient), 1 at its limit, where it fails */
		int hot = 0;
		double hot_f = -1e9;
		for (int i = 0; i < c->count; i++) {
			double f = heat_share(c, i);
			if (f > hot_f)
				hot = i, hot_f = f;
		}
		trs_json_set(j, "heat_part", trs_json_new_str(trs_craft_def(c, hot)->id));
		trs_json_set(j, "heat_temp", trs_json_new_num(c->parts[hot].temp));
		trs_json_set(j, "heat_limit", trs_json_new_num(trs_craft_def(c, hot)->max_temp));
		trs_json_set(j, "heat_frac", trs_json_new_num(hot_f));
	}
	ret_json(r, j);
}

/* Position/velocity of vessel `id` re-expressed in vessel `origin`'s SOI frame
 * and made relative to the origin's CoM (floating origin, doubles). */
static trs_v3 rel_to_origin(const trs_world *w, int id, int origin) {
	trs_v3 r, v, ro, vo;
	trs_vessel_state(w, id, &r, &v);
	trs_vessel_state(w, origin, &ro, &vo);
	trs_rebase_state(&w->sys, w->vessels[id].body, w->vessels[origin].body, w->t, &r, &v);
	return v3_sub(r, ro);
}

#define RPP 14

/* vessel_render(id, origin_id) -> per part: uid, pos(3) rel. origin CoM,
 * quat(4), thrust fraction, chute state, legs, skin temperature (K) and the
 * share of its heat headroom used (0 ambient .. 1 failing), and how far a
 * chute's canopy has opened (radius fraction of full): RPP doubles each. */
static void m_vessel_render(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_vessel(h, a[0].i) || !ok_vessel(h, a[1].i) || !h->world->vessels[a[0].i].craft) {
		ret_doubles(r, NULL, 0);
		return;
	}
	trs_world *w = h->world;
	trs_vessel *vs = &w->vessels[a[0].i];
	trs_craft *c = vs->craft;
	trs_v3 base = rel_to_origin(w, (int)a[0].i, (int)a[1].i);
	trs_quat q = vessel_quat(w, vs);
	double *d = malloc(sizeof(double) * (size_t)(c->count * RPP + 1));
	for (int i = 0; i < c->count; i++) {
		const trs_part *p = &c->parts[i];
		const trs_part_def *def = trs_craft_def(c, i);
		trs_v3 wp = v3_add(base, quat_rotate(q, v3_sub(p->pos, vs->cm)));
		trs_quat wq = quat_mul(q, p->rot);
		double thr = 0;
		if (p->engine_on && def->propellant != TRS_PROP_NONE && trs_craft_engine_fed(c, vs->fuel_group, i))
			thr = def->propellant == TRS_PROP_SOLID ? 1.0 : vs->throttle;
		double *o = d + (size_t)i * RPP;
		o[0] = p->uid;
		o[1] = wp.x, o[2] = wp.y, o[3] = wp.z;
		o[4] = wq.x, o[5] = wq.y, o[6] = wq.z, o[7] = wq.w;
		o[8] = thr;
		o[9] = p->chute;
		o[10] = p->legs_deployed;
		o[11] = p->temp;
		o[12] = heat_share(c, i);
		o[13] = def->is_parachute && def->chute_full_cda > 0 ? sqrt(p->chute_cda / def->chute_full_cda) : 0;
	}
	ret_doubles(r, d, (int64_t)c->count * RPP);
	free(d);
}

/* vessel_parts(id) -> JSON [part ids in render order] (to build visuals) */
static void m_vessel_parts(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	if (ok_vessel(h, a[0].i) && h->world->vessels[a[0].i].craft) {
		trs_craft *c = h->world->vessels[a[0].i].craft;
		for (int i = 0; i < c->count; i++) {
			trs_json *pj = trs_json_new(TRS_JSON_OBJECT);
			trs_json_set(pj, "uid", trs_json_new_num(c->parts[i].uid));
			trs_json_set(pj, "part", trs_json_new_str(trs_craft_def(c, i)->id));
			trs_json_set(pj, "parent", trs_json_new_num(c->parts[i].parent));
			trs_json_set(pj, "node", trs_json_new_str(c->parts[i].node));
			trs_json_set(pj, "parent_node", trs_json_new_str(c->parts[i].parent_node));
			trs_json_push(arr, pj);
		}
	}
	ret_json(r, arr);
}

/* body_render(origin_id) -> per body: pos(3) rel. origin CoM, rotation, radius */
static void m_body_render(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_vessel(h, a[0].i)) {
		ret_doubles(r, NULL, 0);
		return;
	}
	trs_world *w = h->world;
	trs_v3 ro, vo, bo, bvo;
	trs_vessel_state(w, (int)a[0].i, &ro, &vo);
	trs_body_state_abs(&w->sys, w->vessels[a[0].i].body, w->t, &bo, &bvo);
	trs_v3 abs_origin = v3_add(ro, bo);
	double *d = malloc(sizeof(double) * (size_t)(w->sys.count * 5));
	for (int i = 0; i < w->sys.count; i++) {
		trs_v3 bp, bv;
		trs_body_state_abs(&w->sys, i, w->t, &bp, &bv);
		trs_v3 rel = v3_sub(bp, abs_origin);
		d[i * 5] = rel.x, d[i * 5 + 1] = rel.y, d[i * 5 + 2] = rel.z;
		d[i * 5 + 3] = trs_body_rotation(&w->sys.bodies[i], w->t);
		d[i * 5 + 4] = w->sys.bodies[i].radius;
	}
	ret_doubles(r, d, (int64_t)w->sys.count * 5);
	free(d);
}

/* vessel_list() -> JSON [{id, name, body, mode, situation}] */
static void m_vessel_list(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	for (int i = 0; h->world && i < h->world->nvessels; i++) {
		trs_vessel *vs = &h->world->vessels[i];
		if (!vs->alive)
			continue;
		trs_orbit o;
		trs_vessel_orbit(h->world, i, &o);
		trs_json *vj = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(vj, "id", trs_json_new_num(i));
		trs_json_set(vj, "name", trs_json_new_str(vs->craft ? vs->craft->name : "point"));
		trs_json_set(vj, "body", trs_json_new_str(h->world->sys.bodies[vs->body].id));
		trs_json_set(vj, "mode", trs_json_new_str(MODE_NAMES[vs->mode]));
		trs_json_set(vj, "situation", trs_json_new_str(situation(h->world, i, &o)));
		trs_json_set(vj, "crew", trs_json_new_num(0));
		if (vs->craft) {
			int crew = 0;
			for (int k = 0; k < vs->craft->count; k++)
				crew += trs_craft_def(vs->craft, k)->crew;
			trs_json_set(vj, "crew", trs_json_new_num(crew));
			trs_json_set(vj, "parts", trs_json_new_num(vs->craft->count));
		}
		trs_json_push(arr, vj);
	}
	ret_json(r, arr);
}

static const char *EVENT_NAMES[] = { "", "part_destroyed", "vessel_created", "soi_change", "landed", "liftoff",
	"flameout" };

/* events_drain() -> JSON [{kind, t, vessel, other, uid, part, speed}] */
static void m_events_drain(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	for (int i = 0; h->world && i < h->world->nevents; i++) {
		trs_world_event *e = &h->world->events[i];
		trs_json *ej = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(ej, "kind", trs_json_new_str(EVENT_NAMES[e->kind]));
		trs_json_set(ej, "t", trs_json_new_num(e->t));
		trs_json_set(ej, "vessel", trs_json_new_num(e->vessel));
		trs_json_set(ej, "other", trs_json_new_num(e->other));
		trs_json_set(ej, "uid", trs_json_new_num(e->uid));
		trs_json_set(ej, "part", trs_json_new_str(e->part));
		trs_json_set(ej, "speed", trs_json_new_num(e->speed));
		if (e->kind == TRS_WEV_SOI_CHANGE && e->other >= 0)
			trs_json_set(ej, "body", trs_json_new_str(h->world->sys.bodies[e->other].id));
		trs_json_push(arr, ej);
	}
	if (h->world)
		h->world->nevents = 0;
	ret_json(r, arr);
}

/* to_body_fixed(body, origin_vessel, rel[3]) -> body-fixed position of a
 * point given relative to the origin vessel's CoM (for terrain LOD). */
static void m_to_body_fixed(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double out[3] = { 0, 0, 0 };
	if (ok_body(h, a[0].i) && ok_vessel(h, a[1].i) && a[2].arr.n >= 3) {
		trs_world *w = h->world;
		trs_v3 ro, vo, bo, bvo, bp, bv;
		trs_vessel_state(w, (int)a[1].i, &ro, &vo);
		trs_body_state_abs(&w->sys, w->vessels[a[1].i].body, w->t, &bo, &bvo);
		trs_body_state_abs(&w->sys, (int)a[0].i, w->t, &bp, &bv);
		trs_v3 p = v3_add(v3_add(ro, bo), v3(a[2].arr.data[0], a[2].arr.data[1], a[2].arr.data[2]));
		trs_v3 f = trs_body_to_fixed(&w->sys.bodies[a[0].i], w->t, v3_sub(p, bp));
		out[0] = f.x, out[1] = f.y, out[2] = f.z;
	}
	ret_doubles(r, out, 3);
}

/* ground_clearance(body, origin_vessel, rel[3]) -> height (m) of a point
 * given relative to the origin vessel's CoM above the body's terrain: keeps
 * the camera out of hills. */
static void m_ground_clearance(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->f = 1e30;
	if (ok_body(h, a[0].i) && ok_vessel(h, a[1].i) && a[2].arr.n >= 3) {
		trs_world *w = h->world;
		trs_v3 ro, vo, bo, bvo, bp, bv;
		trs_vessel_state(w, (int)a[1].i, &ro, &vo);
		trs_body_state_abs(&w->sys, w->vessels[a[1].i].body, w->t, &bo, &bvo);
		trs_body_state_abs(&w->sys, (int)a[0].i, w->t, &bp, &bv);
		trs_v3 p = v3_sub(v3_add(v3_add(ro, bo), v3(a[2].arr.data[0], a[2].arr.data[1], a[2].arr.data[2])), bp);
		const trs_body *b = &w->sys.bodies[a[0].i];
		double g = trs_body_terrain_at(b, w->t, p);
		r->f = v3_len(p) - b->radius - g;
	}
}

/* terrain_lod(body, eye_fixed[3], max_level, split) -> [face, level, i, j]* */
static void m_terrain_lod(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_body(h, a[0].i) || a[1].arr.n < 3) {
		ret_doubles(r, NULL, 0);
		return;
	}
	const trs_body *b = &h->world->sys.bodies[a[0].i];
	enum { MAXP = 2048 };
	trs_patch_id *ids = malloc(sizeof(trs_patch_id) * MAXP);
	int n = trs_terrain_lod(&b->terrain, b->radius, v3(a[1].arr.data[0], a[1].arr.data[1], a[1].arr.data[2]),
			(int)a[2].i, a[3].f, ids, MAXP);
	double *d = malloc(sizeof(double) * (size_t)(n * 4 + 1));
	for (int k = 0; k < n; k++) {
		d[k * 4] = ids[k].face, d[k * 4 + 1] = ids[k].level, d[k * 4 + 2] = ids[k].i, d[k * 4 + 3] = ids[k].j;
	}
	ret_doubles(r, d, (int64_t)n * 4);
	free(d);
	free(ids);
}

/* terrain_patch(body, [face, level, i, j], res) -> [cx,cy,cz, pos(3n), nrm(3n)] */
static void m_terrain_patch(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_body(h, a[0].i) || a[1].arr.n < 4) {
		ret_doubles(r, NULL, 0);
		return;
	}
	const trs_body *b = &h->world->sys.bodies[a[0].i];
	int res = (int)a[2].i;
	if (res < 2) res = 2;
	if (res > 129) res = 129;
	trs_patch_id p = { (int)a[1].arr.data[0], (int)a[1].arr.data[1], (int)a[1].arr.data[2], (int)a[1].arr.data[3] };
	int maxv = res * res + 4 * res;
	double *pos = malloc(sizeof(double) * (size_t)maxv * 3), *nrm = malloc(sizeof(double) * (size_t)maxv * 3);
	trs_v3 c;
	int nv = trs_terrain_patch_mesh(&b->terrain, b->radius, p, res, pos, nrm, &c);
	int64_t total = 3 + (int64_t)nv * 6;
	double *d = malloc(sizeof(double) * (size_t)total);
	d[0] = c.x, d[1] = c.y, d[2] = c.z;
	memcpy(d + 3, pos, sizeof(double) * (size_t)nv * 3);
	memcpy(d + 3 + nv * 3, nrm, sizeof(double) * (size_t)nv * 3);
	ret_doubles(r, d, total);
	free(d);
	free(pos);
	free(nrm);
}

/* sas_direction(id, mode) -> [x,y,z] for HUD markers */
static void m_sas_direction(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double out[3] = { 0, 0, 0 };
	if (ok_vessel(h, a[0].i))
		for (int m = 0; m < TRS_SAS_COUNT; m++)
			if (!strcmp(SAS_NAMES[m], a[1].s)) {
				trs_v3 d = trs_sas_direction(h->world, (int)a[0].i, (trs_sas_mode)m);
				out[0] = d.x, out[1] = d.y, out[2] = d.z;
			}
	ret_doubles(r, out, 3);
}

/* ---- maneuver nodes / trajectories ----------------------------------------- */

static int read_nodes(const trs_val *a, trs_node *nodes, int max) {
	int n = (int)(a->arr.n / 4);
	if (n > max)
		n = max;
	for (int k = 0; k < n; k++) {
		nodes[k].t = a->arr.data[k * 4];
		nodes[k].pro = a->arr.data[k * 4 + 1];
		nodes[k].nrm = a->arr.data[k * 4 + 2];
		nodes[k].rad = a->arr.data[k * 4 + 3];
	}
	return n;
}

static int vessel_trajectory(trs_sim_handle *h, int id, const trs_val *nodes_arr, int max_patches, double horizon,
		trs_patch *out) {
	trs_node nodes[32];
	int nn = read_nodes(nodes_arr, nodes, 32);
	trs_orbit o;
	trs_vessel_orbit(h->world, id, &o);
	return trs_trajectory(&h->world->sys, h->world->vessels[id].body, &o, h->world->t, nodes, nn, horizon, out,
			max_patches);
}

static const char *PATCH_END[] = { "horizon", "soi_exit", "soi_enter", "atmosphere", "surface", "node" };

/* trajectory(id, nodes[t,pro,nrm,rad]*, max_patches, horizon) -> JSON patches */
static void m_trajectory(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	if (ok_vessel(h, a[0].i)) {
		trs_patch patches[32];
		int max = (int)a[2].i < 32 ? (int)a[2].i : 32;
		int n = vessel_trajectory(h, (int)a[0].i, &a[1], max, a[3].f, patches);
		for (int k = 0; k < n; k++) {
			trs_patch *p = &patches[k];
			const trs_body *b = &h->world->sys.bodies[p->body];
			trs_json *pj = trs_json_new(TRS_JSON_OBJECT);
			int closed = trs_orbit_is_closed(&p->orbit);
			trs_json_set(pj, "body", trs_json_new_str(b->id));
			trs_json_set(pj, "body_index", trs_json_new_num(p->body));
			trs_json_set(pj, "t_start", trs_json_new_num(p->t0));
			trs_json_set(pj, "t_end", trs_json_new_num(p->t1));
			trs_json_set(pj, "end", trs_json_new_str(PATCH_END[p->end]));
			trs_json_set(pj, "node", trs_json_new_num(p->node));
			trs_json_set(pj, "sma", trs_json_new_num(p->orbit.a));
			trs_json_set(pj, "ecc", trs_json_new_num(p->orbit.e));
			trs_json_set(pj, "inc_deg", trs_json_new_num(p->orbit.inc * DEG));
			trs_json_set(pj, "pe_alt", trs_json_new_num(p->orbit.rp - b->radius));
			trs_json_set(pj, "ap_alt", closed ? trs_json_new_num(p->orbit.ra - b->radius) : trs_json_new(TRS_JSON_NULL));
			double tpe = trs_orbit_next_periapsis(&p->orbit, p->t0);
			trs_json_set(pj, "pe_t", tpe <= p->t1 ? trs_json_new_num(tpe) : trs_json_new(TRS_JSON_NULL));
			double tap = trs_orbit_next_apoapsis(&p->orbit, p->t0);
			trs_json_set(pj, "ap_t", tap <= p->t1 ? trs_json_new_num(tap) : trs_json_new(TRS_JSON_NULL));
			trs_json_set(pj, "period", trs_json_new_num(p->orbit.period));
			/* exact marker positions relative to the patch body (sim axes) */
			trs_v3 rr, vv;
			trs_orbit_state_at(&p->orbit, p->t1, &rr, &vv);
			trs_json_set(pj, "end_pos", v3_json(rr));
			trs_orbit_state_at(&p->orbit, p->t0, &rr, &vv);
			trs_json_set(pj, "start_pos", v3_json(rr));
			if (tpe <= p->t1) {
				trs_orbit_state_at(&p->orbit, tpe, &rr, &vv);
				trs_json_set(pj, "pe_pos", v3_json(rr));
			}
			if (tap <= p->t1) {
				trs_orbit_state_at(&p->orbit, tap, &rr, &vv);
				trs_json_set(pj, "ap_pos", v3_json(rr));
			}
			trs_json_push(arr, pj);
		}
	}
	ret_json(r, arr);
}

/* trajectory_points(id, nodes, max_patches, horizon, samples) ->
 * [patch, t, x, y, z]* positions relative to each patch's body (SOI frame),
 * sampled evenly in true anomaly so periapses stay smooth. */
static void m_trajectory_points(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_vessel(h, a[0].i)) {
		ret_doubles(r, NULL, 0);
		return;
	}
	trs_patch patches[32];
	int max = (int)a[2].i < 32 ? (int)a[2].i : 32;
	int n = vessel_trajectory(h, (int)a[0].i, &a[1], max, a[3].f, patches);
	int samples = (int)a[4].i < 8 ? 8 : (int)a[4].i > 2048 ? 2048 : (int)a[4].i;
	double *d = malloc(sizeof(double) * (size_t)(n * (samples + 1) * 5 + 1));
	int k = 0;
	for (int pi = 0; pi < n; pi++) {
		trs_patch *p = &patches[pi];
		const trs_orbit *o = &p->orbit;
		double nu0 = trs_orbit_true_anomaly_at(o, p->t0);
		double span;
		if (trs_orbit_is_closed(o) && (p->t1 - p->t0) >= o->period * 0.999) {
			span = TRS_TAU;
		} else {
			double nu1 = trs_orbit_true_anomaly_at(o, p->t1);
			span = nu1 - nu0;
			if (trs_orbit_is_closed(o))
				span = trs_wrap_tau(span);
			if (span <= 0)
				span += trs_orbit_is_closed(o) ? TRS_TAU : 0;
		}
		for (int s = 0; s <= samples; s++) {
			double nu = nu0 + span * s / samples;
			trs_v3 pos = trs_orbit_position_at_true(o, nu);
			double t = s == 0 ? p->t0 : trs_orbit_time_at_true(o, p->t0, nu);
			if (!isfinite(t) || t > p->t1)
				t = p->t1;
			double *row = d + (size_t)k * 5;
			row[0] = pi, row[1] = t, row[2] = pos.x, row[3] = pos.y, row[4] = pos.z;
			k++;
		}
	}
	ret_doubles(r, d, (int64_t)k * 5);
	free(d);
}

/* node_vector(id, nodes, index) -> [x,y,z] SOI-frame delta-v of node `index`
 * on the predicted trajectory (for steering a burn). */
static void m_node_vector(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double out[3] = { 0, 0, 0 };
	if (ok_vessel(h, a[0].i)) {
		trs_node nodes[32];
		int nn = read_nodes(&a[1], nodes, 32);
		trs_patch patches[32];
		int n = vessel_trajectory(h, (int)a[0].i, &a[1], 32, 1e9, patches);
		for (int k = 0; k + 1 < n; k++)
			if (patches[k].end == TRS_PATCH_END_NODE && patches[k + 1].node == a[2].i && a[2].i < nn) {
				trs_v3 v = trs_node_vector(&patches[k].orbit, &nodes[a[2].i]);
				out[0] = v.x, out[1] = v.y, out[2] = v.z;
			}
	}
	ret_doubles(r, out, 3);
}

/* burn_time(id, dv) -> seconds at full throttle with the current stage
 * (Tsiolkovsky with mass flow), or -1 if no lit engines. */
static void m_burn_time(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->f = -1;
	if (!ok_vessel(h, a[0].i) || !h->world->vessels[a[0].i].craft)
		return;
	trs_vessel *vs = &h->world->vessels[a[0].i];
	trs_craft *c = vs->craft;
	double mdot = 0, f = 0;
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		if (!c->parts[i].engine_on || d->propellant == TRS_PROP_NONE || !trs_craft_engine_fed(c, vs->fuel_group, i))
			continue;
		mdot += trs_engine_mdot(d);
		f += d->thrust_vac;
	}
	if (mdot <= 0) {
		/* nothing lit: estimate with the next stage's engines */
		for (int i = 0; i < c->count; i++) {
			const trs_part_def *d = trs_craft_def(c, i);
			if (c->parts[i].stage == vs->next_stage && d->propellant != TRS_PROP_NONE) {
				mdot += trs_engine_mdot(d);
				f += d->thrust_vac;
			}
		}
	}
	if (mdot <= 0)
		return;
	double ve = f / mdot;
	r->f = vs->mass * (1 - exp(-a[1].f / ve)) / mdot;
}

/* flight_place_landed(craft, body, lat_deg, lon_deg) -> id resting on the
 * surface (no clamps; LANDED until something moves it). */
static void m_flight_place_landed(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = -1;
	trs_craft c;
	if (!h->world || !craft_from_text(h, a[0].s, &c))
		return;
	int b = trs_system_find(&h->world->sys, a[1].s);
	if (b >= 0) {
		r->i = trs_world_launch(h->world, &c, b, a[2].f / DEG, a[3].f / DEG);
		if (r->i >= 0)
			h->world->vessels[r->i].clamped = 0;
	}
	trs_craft_free(&c);
}

/* subpoint(body, target) -> [lat_deg, lon_deg] on `body` where `target`
 * body is overhead now */
static void m_subpoint(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double out[2] = { 0, 0 };
	if (ok_body(h, a[0].i) && ok_body(h, a[1].i)) {
		trs_v3 bp, bv, tp, tv;
		trs_body_state_abs(&h->world->sys, (int)a[0].i, h->world->t, &bp, &bv);
		trs_body_state_abs(&h->world->sys, (int)a[1].i, h->world->t, &tp, &tv);
		trs_v3 d = v3_norm(trs_body_to_fixed(&h->world->sys.bodies[a[0].i], h->world->t, v3_sub(tp, bp)));
		out[0] = asin(d.z) * DEG;
		out[1] = atan2(d.y, d.x) * DEG;
	}
	ret_doubles(r, out, 2);
}

/* subsolar_point(body) -> [lat_deg, lon_deg] where the star is overhead now */
static void m_subsolar_point(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double out[2] = { 0, 0 };
	if (ok_body(h, a[0].i)) {
		trs_v3 bp, bv;
		trs_body_state_abs(&h->world->sys, (int)a[0].i, h->world->t, &bp, &bv);
		trs_v3 d = v3_norm(trs_body_to_fixed(&h->world->sys.bodies[a[0].i], h->world->t, v3_neg(bp)));
		out[0] = asin(d.z) * DEG;
		out[1] = atan2(d.y, d.x) * DEG;
	}
	ret_doubles(r, out, 2);
}

/* terrain_map(body, w, h) -> w*h heights (m) on an equirectangular grid in
 * the body-fixed frame: x -> longitude -pi..pi, y -> latitude +pi/2..-pi/2 */
static void m_terrain_map(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	int w = (int)a[1].i, hh = (int)a[2].i;
	if (!ok_body(h, a[0].i) || w < 2 || hh < 2 || w > 8192 || hh > 4096) {
		ret_doubles(r, NULL, 0);
		return;
	}
	const trs_body *b = &h->world->sys.bodies[a[0].i];
	double *d = malloc(sizeof(double) * (size_t)w * (size_t)hh);
	for (int y = 0; y < hh; y++) {
		double lat = TRS_PI / 2 - (y + 0.5) / hh * TRS_PI;
		for (int x = 0; x < w; x++) {
			double lon = (x + 0.5) / w * TRS_TAU - TRS_PI;
			trs_v3 dir = v3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
			d[(size_t)y * w + x] = trs_terrain_height(&b->terrain, b->radius, dir);
		}
	}
	ret_doubles(r, d, (int64_t)w * hh);
	free(d);
}

/* flight_spawn_state(craft, body, [rx,ry,rz,vx,vy,vz]) -> id, nose along v */
static void m_flight_spawn_state(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = -1;
	trs_craft c;
	if (!h->world || a[2].arr.n < 6 || !craft_from_text(h, a[0].s, &c))
		return;
	int b = trs_system_find(&h->world->sys, a[1].s);
	if (b >= 0) {
		const double *d = a[2].arr.data;
		trs_v3 pos = v3(d[0], d[1], d[2]), vel = v3(d[3], d[4], d[5]);
		trs_quat q = quat_from_to(v3(0, 1, 0), v3_norm(vel));
		r->i = trs_world_add_craft(h->world, &c, b, pos, vel, q);
	}
	trs_craft_free(&c);
}

#define S TRS_T_STRING
#define I TRS_T_INT
#define F TRS_T_FLOAT
#define A TRS_T_F64ARR
#define B TRS_T_BOOL
#define V TRS_T_VOID
const trs_method_def TRS_SIM_FLIGHT_METHODS[] = {
	{ "flight_launch", m_flight_launch, I, 2, { S, S }, { "craft", "body" } },
	{ "flight_spawn_orbit", m_flight_spawn_orbit, I, 4, { S, S, F, F }, { "craft", "body", "altitude", "inc_deg" } },
	{ "vessel_set_throttle", m_vessel_set_throttle, V, 2, { I, F }, { "id", "throttle" } },
	{ "vessel_set_input", m_vessel_set_input, V, 4, { I, F, F, F }, { "id", "pitch", "yaw", "roll" } },
	{ "vessel_set_sas", m_vessel_set_sas, B, 3, { I, S, A }, { "id", "mode", "target" } },
	{ "vessel_stage", m_vessel_stage, S, 1, { I }, { "id" } },
	{ "vessel_set_legs", m_vessel_set_legs, V, 2, { I, B }, { "id", "deployed" } },
	{ "vessel_deploy_chutes", m_vessel_deploy_chutes, I, 1, { I }, { "id" } },
	{ "vessel_set_mode", m_vessel_set_mode, V, 2, { I, S }, { "id", "mode" } },
	{ "vessel_alive", m_vessel_alive, B, 1, { I }, { "id" } },
	{ "telemetry", m_telemetry, S, 1, { I }, { "id" } },
	{ "vessel_render", m_vessel_render, A, 2, { I, I }, { "id", "origin" } },
	{ "vessel_parts", m_vessel_parts, S, 1, { I }, { "id" } },
	{ "body_render", m_body_render, A, 1, { I }, { "origin" } },
	{ "vessel_list", m_vessel_list, S, 0, { 0 }, { 0 } },
	{ "events_drain", m_events_drain, S, 0, { 0 }, { 0 } },
	{ "to_body_fixed", m_to_body_fixed, A, 3, { I, I, A }, { "body", "origin", "rel" } },
	{ "ground_clearance", m_ground_clearance, F, 3, { I, I, A }, { "body", "origin", "rel" } },
	{ "terrain_lod", m_terrain_lod, A, 4, { I, A, I, F }, { "body", "eye", "max_level", "split" } },
	{ "terrain_patch", m_terrain_patch, A, 3, { I, A, I }, { "body", "patch", "res" } },
	{ "sas_direction", m_sas_direction, A, 2, { I, S }, { "id", "mode" } },
	{ "trajectory", m_trajectory, S, 4, { I, A, I, F }, { "id", "nodes", "max_patches", "horizon" } },
	{ "trajectory_points", m_trajectory_points, A, 5, { I, A, I, F, I }, { "id", "nodes", "max_patches", "horizon", "samples" } },
	{ "node_vector", m_node_vector, A, 3, { I, A, I }, { "id", "nodes", "index" } },
	{ "burn_time", m_burn_time, F, 2, { I, F }, { "id", "dv" } },
	{ "flight_place_landed", m_flight_place_landed, I, 4, { S, S, F, F }, { "craft", "body", "lat_deg", "lon_deg" } },
	{ "subsolar_point", m_subsolar_point, A, 1, { I }, { "body" } },
	{ "subpoint", m_subpoint, A, 2, { I, I }, { "body", "target" } },
	{ "flight_spawn_state", m_flight_spawn_state, I, 3, { S, S, A }, { "craft", "body", "state" } },
	{ "terrain_map", m_terrain_map, A, 3, { I, I, I }, { "body", "w", "h" } },
};
const int TRS_SIM_FLIGHT_METHOD_COUNT = sizeof(TRS_SIM_FLIGHT_METHODS) / sizeof(TRS_SIM_FLIGHT_METHODS[0]);
