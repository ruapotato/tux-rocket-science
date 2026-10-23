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

#include "trs/world.h"

#include <math.h>
#include <string.h>

static void time_add(trs_world *w, double dt) {
	double y = dt - w->t_comp;
	double t = w->t + y;
	w->t_comp = (t - w->t) - y;
	w->t = t;
}

static void time_set(trs_world *w, double t) {
	w->t = t;
	w->t_comp = 0;
}

void trs_world_init(trs_world *w, const trs_system *sys, double t0) {
	memset(w, 0, sizeof(*w));
	w->sys = *sys;
	time_set(w, t0);
}

int trs_world_add_vessel(trs_world *w, int body, trs_v3 r, trs_v3 v) {
	int id = -1;
	for (int i = 0; i < w->nvessels; i++)
		if (!w->vessels[i].alive) {
			id = i;
			break;
		}
	if (id < 0) {
		if (w->nvessels >= TRS_MAX_VESSELS)
			return -1;
		id = w->nvessels++;
	}
	trs_vessel *vs = &w->vessels[id];
	memset(vs, 0, sizeof(*vs));
	vs->alive = 1;
	vs->body = body;
	vs->mode = TRS_MODE_PHYSICS;
	vs->r = r;
	vs->v = v;
	return id;
}

void trs_world_remove_vessel(trs_world *w, int id) {
	w->vessels[id].alive = 0;
}

void trs_vessel_state(const trs_world *w, int id, trs_v3 *r, trs_v3 *v) {
	const trs_vessel *vs = &w->vessels[id];
	if (vs->mode == TRS_MODE_RAILS) {
		trs_orbit_state_at(&vs->orbit, w->t, r, v);
	} else {
		*r = vs->r;
		*v = vs->v;
	}
}

void trs_vessel_orbit(const trs_world *w, int id, trs_orbit *o) {
	const trs_vessel *vs = &w->vessels[id];
	if (vs->mode == TRS_MODE_RAILS) {
		*o = vs->orbit;
		return;
	}
	trs_orbit_from_state(o, w->sys.bodies[vs->body].mu, vs->r, vs->v, w->t);
}

void trs_vessel_set_mode(trs_world *w, int id, trs_mode mode) {
	trs_vessel *vs = &w->vessels[id];
	if (vs->mode == mode)
		return;
	if (mode == TRS_MODE_RAILS) {
		trs_orbit_from_state(&vs->orbit, w->sys.bodies[vs->body].mu, vs->r, vs->v, w->t);
	} else {
		trs_orbit_state_at(&vs->orbit, w->t, &vs->r, &vs->v);
	}
	vs->mode = mode;
}

void trs_vessel_set_state(trs_world *w, int id, int body, trs_v3 r, trs_v3 v) {
	trs_vessel *vs = &w->vessels[id];
	vs->body = body;
	vs->r = r;
	vs->v = v;
	if (vs->mode == TRS_MODE_RAILS)
		trs_orbit_from_state(&vs->orbit, w->sys.bodies[body].mu, r, v, w->t);
}

void trs_rebase_state(const trs_system *sys, int from, int to, double t, trs_v3 *r, trs_v3 *v) {
	if (from == to)
		return;
	trs_v3 rf, vf, rt, vt;
	/* Fast exact paths for the common parent/child hops. */
	if (sys->bodies[from].parent == to) {
		trs_body_state(sys, from, t, &rf, &vf);
		*r = v3_add(*r, rf);
		*v = v3_add(*v, vf);
		return;
	}
	if (sys->bodies[to].parent == from) {
		trs_body_state(sys, to, t, &rt, &vt);
		*r = v3_sub(*r, rt);
		*v = v3_sub(*v, vt);
		return;
	}
	trs_body_state_abs(sys, from, t, &rf, &vf);
	trs_body_state_abs(sys, to, t, &rt, &vt);
	*r = v3_sub(v3_add(*r, rf), rt);
	*v = v3_sub(v3_add(*v, vf), vt);
}

/* ---- event search ------------------------------------------------------ */

/* Signed clearance from child c's (entry-shrunk) SOI at time t. */
static double child_clearance(const trs_system *sys, int c, const trs_orbit *o, double t) {
	trs_v3 rv, vv, rc, vc;
	trs_orbit_state_at(o, t, &rv, &vv);
	trs_body_state(sys, c, t, &rc, &vc);
	return v3_len(v3_sub(rv, rc)) - sys->bodies[c].soi * TRS_SOI_ENTER_FACTOR;
}

/* Earliest time in [t0, t1] at which the orbit enters child c's SOI, using
 * conservative advancement: clearance can shrink no faster than the maximum
 * relative speed, so a step of clearance/vmax can never skip an entry. */
static double find_child_entry(const trs_system *sys, int c, const trs_orbit *o, double t0, double t1) {
	const trs_body *cb = &sys->bodies[c];
	double vmax = trs_orbit_max_speed(o) + trs_orbit_max_speed(&cb->orbit);
	const double hmin = 1e-2;
	double t = t0, d = child_clearance(sys, c, o, t);
	if (d <= 0)
		return t0;
	for (int it = 0; it < 2000000 && t < t1; it++) {
		double h = fmax(d / vmax, hmin);
		double tn = fmin(t + h, t1);
		double dn = child_clearance(sys, c, o, tn);
		if (dn <= 0) {
			double lo = t, hi = tn; /* d(lo) > 0 >= d(hi) */
			for (int k = 0; k < 80 && hi - lo > 1e-9 * fmax(1.0, fabs(hi)); k++) {
				double mid = 0.5 * (lo + hi);
				if (child_clearance(sys, c, o, mid) > 0)
					lo = mid;
				else
					hi = mid;
			}
			return hi;
		}
		t = tn;
		d = dn;
	}
	return INFINITY;
}

trs_event trs_next_event(const trs_system *sys, int body, const trs_orbit *o, double t, double t_max) {
	trs_event ev = { TRS_EV_NONE, INFINITY, body };
	const trs_body *b = &sys->bodies[body];

	/* Atmosphere top, or the surface of an airless body. */
	double low = b->radius + (b->atm.height > 0 ? b->atm.height : 0);
	double t_low = trs_orbit_next_inbound_crossing(o, t, low);
	if (t_low <= t_max) {
		ev.kind = b->atm.height > 0 ? TRS_EV_ATMOSPHERE : TRS_EV_SURFACE;
		ev.t = t_low;
		ev.body = body;
	}
	if (b->parent >= 0) {
		double t_exit = trs_orbit_next_outbound_crossing(o, t, b->soi * TRS_SOI_EXIT_FACTOR);
		if (t_exit <= t_max && t_exit < ev.t) {
			ev.kind = TRS_EV_SOI_EXIT;
			ev.t = t_exit;
			ev.body = b->parent;
		}
	}
	double horizon = fmin(ev.t, t_max);
	for (int k = 0; k < b->nchildren; k++) {
		int c = b->children[k];
		double tc = find_child_entry(sys, c, o, t, horizon);
		if (tc <= horizon && tc < ev.t) {
			ev.kind = TRS_EV_SOI_ENTER;
			ev.t = tc;
			ev.body = c;
			horizon = tc;
		}
	}
	return ev;
}

/* ---- physics ----------------------------------------------------------- */

typedef struct {
	trs_v3 dr, dv;
} deriv;

static deriv eval(const trs_world *w, const trs_vessel *vs, double t, trs_v3 r, trs_v3 v) {
	double mu = w->sys.bodies[vs->body].mu;
	double rl2 = v3_len2(r), rl = sqrt(rl2);
	deriv d;
	d.dr = v;
	d.dv = v3_scale(r, -mu / (rl2 * rl));
	if (vs->accel)
		d.dv = v3_add(d.dv, vs->accel(vs->accel_user, vs->body, t, r, v));
	return d;
}

static void rk4(const trs_world *w, trs_vessel *vs, double t, double h) {
	trs_v3 r = vs->r, v = vs->v;
	deriv k1 = eval(w, vs, t, r, v);
	deriv k2 = eval(w, vs, t + 0.5 * h, v3_madd(r, k1.dr, 0.5 * h), v3_madd(v, k1.dv, 0.5 * h));
	deriv k3 = eval(w, vs, t + 0.5 * h, v3_madd(r, k2.dr, 0.5 * h), v3_madd(v, k2.dv, 0.5 * h));
	deriv k4 = eval(w, vs, t + h, v3_madd(r, k3.dr, h), v3_madd(v, k3.dv, h));
	trs_v3 sr = v3_add(v3_add(k1.dr, v3_scale(k2.dr, 2)), v3_add(v3_scale(k3.dr, 2), k4.dr));
	trs_v3 sv = v3_add(v3_add(k1.dv, v3_scale(k2.dv, 2)), v3_add(v3_scale(k3.dv, 2), k4.dv));
	vs->r = v3_madd(r, sr, h / 6.0);
	vs->v = v3_madd(v, sv, h / 6.0);
}

/* Moves a physics vessel across SOI boundaries after a tick. */
static void physics_soi_check(trs_world *w, trs_vessel *vs) {
	for (int guard = 0; guard < 4; guard++) {
		const trs_body *b = &w->sys.bodies[vs->body];
		if (b->parent >= 0 && v3_len(vs->r) > b->soi * TRS_SOI_EXIT_FACTOR) {
			trs_rebase_state(&w->sys, vs->body, b->parent, w->t, &vs->r, &vs->v);
			vs->body = b->parent;
			vs->soi_changes++;
			continue;
		}
		int entered = 0;
		for (int k = 0; k < b->nchildren; k++) {
			int c = b->children[k];
			trs_v3 rc, vc;
			trs_body_state(&w->sys, c, w->t, &rc, &vc);
			if (v3_len(v3_sub(vs->r, rc)) < w->sys.bodies[c].soi * TRS_SOI_ENTER_FACTOR) {
				trs_rebase_state(&w->sys, vs->body, c, w->t, &vs->r, &vs->v);
				vs->body = c;
				vs->soi_changes++;
				entered = 1;
				break;
			}
		}
		if (!entered)
			return;
	}
}

void trs_world_step_physics(trs_world *w) {
	const double h = TRS_PHYSICS_DT;
	double t = w->t;
	for (int i = 0; i < w->nvessels; i++) {
		trs_vessel *vs = &w->vessels[i];
		if (vs->alive && vs->mode == TRS_MODE_PHYSICS)
			rk4(w, vs, t, h);
	}
	time_add(w, h);
	for (int i = 0; i < w->nvessels; i++) {
		trs_vessel *vs = &w->vessels[i];
		if (vs->alive && vs->mode == TRS_MODE_PHYSICS)
			physics_soi_check(w, vs);
	}
}

/* ---- rails warp -------------------------------------------------------- */

/* Advances one rails vessel to t_end through its SOI transitions. Returns
 * the first atmosphere/surface event if `stop_at_low`, else NONE. */
static trs_event advance_rails(trs_world *w, trs_vessel *vs, double t_start, double t_end, int stop_at_low) {
	double t = t_start;
	trs_event none = { TRS_EV_NONE, t_end, vs->body };
	for (int guard = 0; guard < 64; guard++) {
		trs_event ev = trs_next_event(&w->sys, vs->body, &vs->orbit, t, t_end);
		if (ev.kind == TRS_EV_NONE)
			return none;
		if (ev.kind == TRS_EV_ATMOSPHERE || ev.kind == TRS_EV_SURFACE) {
			if (stop_at_low)
				return ev;
			return none; /* background vessels just keep coasting */
		}
		trs_v3 r, v;
		trs_orbit_state_at(&vs->orbit, ev.t, &r, &v);
		trs_rebase_state(&w->sys, vs->body, ev.body, ev.t, &r, &v);
		vs->body = ev.body;
		trs_orbit_from_state(&vs->orbit, w->sys.bodies[ev.body].mu, r, v, ev.t);
		vs->soi_changes++;
		t = ev.t;
	}
	return none;
}

void trs_world_warp(trs_world *w, double dt, int stop_on, trs_event *stop) {
	double t0 = w->t, t1 = w->t + dt;
	trs_event result = { TRS_EV_NONE, t1, -1 };
	if (stop_on >= 0 && w->vessels[stop_on].alive && w->vessels[stop_on].mode == TRS_MODE_RAILS) {
		trs_event ev = advance_rails(w, &w->vessels[stop_on], t0, t1, 1);
		if (ev.kind != TRS_EV_NONE) {
			result = ev;
			t1 = ev.t;
		}
	}
	for (int i = 0; i < w->nvessels; i++) {
		trs_vessel *vs = &w->vessels[i];
		if (!vs->alive || vs->mode != TRS_MODE_RAILS || i == stop_on)
			continue;
		advance_rails(w, vs, t0, t1, 0);
	}
	time_set(w, t1);
	if (stop)
		*stop = result;
}
