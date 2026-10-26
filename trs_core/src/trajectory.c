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

#include "trs/trajectory.h"

#include <math.h>
#include <stdlib.h>

trs_v3 trs_node_vector(const trs_orbit *o, const trs_node *n) {
	trs_v3 r, v;
	trs_orbit_state_at(o, n->t, &r, &v);
	trs_v3 pro = v3_norm(v);
	trs_v3 nrm = v3_norm(v3_cross(r, v));
	trs_v3 rad = v3_cross(pro, nrm); /* radial-out: in plane, away from the body */
	rad = v3_neg(rad);
	if (v3_dot(rad, r) < 0)
		rad = v3_neg(rad);
	return v3_add(v3_add(v3_scale(pro, n->pro), v3_scale(nrm, n->nrm)), v3_scale(rad, n->rad));
}

int trs_trajectory(const trs_system *sys, int body, const trs_orbit *o0, double t, const trs_node *nodes, int nn,
		double horizon, trs_patch *out, int max) {
	trs_orbit o = *o0;
	double t_max = t + horizon;
	int used[64] = { 0 };
	int n = 0, start_node = -1;
	while (n < max) {
		/* next unused node after t */
		int ni = -1;
		for (int k = 0; k < nn && k < 64; k++)
			if (!used[k] && nodes[k].t >= t && (ni < 0 || nodes[k].t < nodes[ni].t))
				ni = k;
		double limit = ni >= 0 ? fmin(nodes[ni].t, t_max) : t_max;
		trs_event ev = trs_next_event(sys, body, &o, t, limit);
		trs_patch *p = &out[n++];
		p->body = body;
		p->orbit = o;
		p->t0 = t;
		p->node = start_node;
		start_node = -1;
		if (ev.kind != TRS_EV_NONE) {
			p->t1 = ev.t;
			p->end = ev.kind == TRS_EV_SOI_EXIT ? TRS_PATCH_END_SOI_EXIT
					: ev.kind == TRS_EV_SOI_ENTER ? TRS_PATCH_END_SOI_ENTER
					: ev.kind == TRS_EV_ATMOSPHERE ? TRS_PATCH_END_ATMOSPHERE
												 : TRS_PATCH_END_SURFACE;
			if (p->end == TRS_PATCH_END_ATMOSPHERE || p->end == TRS_PATCH_END_SURFACE)
				break;
			trs_v3 r, v;
			trs_orbit_state_at(&o, ev.t, &r, &v);
			trs_rebase_state(sys, body, ev.body, ev.t, &r, &v);
			body = ev.body;
			trs_orbit_from_state(&o, sys->bodies[body].mu, r, v, ev.t);
			t = ev.t;
			continue;
		}
		if (ni >= 0 && nodes[ni].t <= t_max) {
			p->t1 = nodes[ni].t;
			p->end = TRS_PATCH_END_NODE;
			used[ni] = 1;
			trs_v3 r, v;
			trs_orbit_state_at(&o, nodes[ni].t, &r, &v);
			v = v3_add(v, trs_node_vector(&o, &nodes[ni]));
			trs_orbit_from_state(&o, sys->bodies[body].mu, r, v, nodes[ni].t);
			t = nodes[ni].t;
			start_node = ni;
			continue;
		}
		p->t1 = t_max;
		p->end = TRS_PATCH_END_HORIZON;
		break;
	}
	return n;
}

/* ---- closest approach ------------------------------------------------------- */

#define CA_SAMPLES 720

static double ca_dist(const trs_orbit *a, const trs_orbit *b, double t) {
	trs_v3 ra, va, rb, vb;
	trs_orbit_state_at(a, t, &ra, &va);
	trs_orbit_state_at(b, t, &rb, &vb);
	return v3_len(v3_sub(ra, rb));
}

/* Sampled over each overlap of same-body patches, then the best sample's
 * bracket is narrowed by golden section to a millisecond: the true minimum
 * of the distance between the two conics there. */
int trs_closest_approach(const trs_patch *a, int na, const trs_patch *b, int nb, double t0, double t1, trs_approach *out) {
	int found = 0;
	out->dist = INFINITY;
	for (int i = 0; i < na; i++)
		for (int j = 0; j < nb; j++) {
			if (a[i].body != b[j].body)
				continue;
			double lo = fmax(t0, fmax(a[i].t0, b[j].t0)), hi = fmin(t1, fmin(a[i].t1, b[j].t1));
			if (hi <= lo)
				continue;
			double step = (hi - lo) / CA_SAMPLES, best_t = lo, best = INFINITY;
			for (int k = 0; k <= CA_SAMPLES; k++) {
				double t = lo + step * k, d = ca_dist(&a[i].orbit, &b[j].orbit, t);
				if (d < best)
					best = d, best_t = t;
			}
			double x0 = fmax(lo, best_t - step), x1 = fmin(hi, best_t + step);
			const double gr = 0.6180339887498949;
			double c = x1 - gr * (x1 - x0), d = x0 + gr * (x1 - x0);
			double fc = ca_dist(&a[i].orbit, &b[j].orbit, c), fd = ca_dist(&a[i].orbit, &b[j].orbit, d);
			for (int it = 0; it < 200 && x1 - x0 > 1e-3; it++) {
				if (fc < fd) {
					x1 = d, d = c, fd = fc;
					c = x1 - gr * (x1 - x0);
					fc = ca_dist(&a[i].orbit, &b[j].orbit, c);
				} else {
					x0 = c, c = d, fc = fd;
					d = x0 + gr * (x1 - x0);
					fd = ca_dist(&a[i].orbit, &b[j].orbit, d);
				}
			}
			double t = 0.5 * (x0 + x1), dist = ca_dist(&a[i].orbit, &b[j].orbit, t);
			if (best < dist)
				t = best_t, dist = best; /* an end of the overlap */
			if (dist < out->dist) {
				trs_v3 va, vb;
				trs_orbit_state_at(&a[i].orbit, t, &out->ra, &va);
				trs_orbit_state_at(&b[j].orbit, t, &out->rb, &vb);
				out->t = t, out->dist = dist, out->body = a[i].body;
				out->rel_speed = v3_len(v3_sub(va, vb));
				found = 1;
			}
		}
	return found;
}

/* ---- transfer windows ------------------------------------------------------- */

/* Between two bodies circling the same parent: the Hohmann transfer from
 * `from`'s orbit to `to`'s. The phase angle is how far `to` must lead
 * `from` (measured in their orbital plane, prograde) at departure; the
 * window is when it next does. Circular, coplanar orbits are assumed
 * (both at their semi-major axes) for the best lead and the burn; the
 * moment it comes round uses the bodies' real positions. */
/* how far body `to` leads `from` round their parent at time t, rad */
static double lead_at(const trs_system *sys, int from, int to, double t) {
	trs_v3 ra, va, rb, vb;
	trs_body_state(sys, from, t, &ra, &va);
	trs_body_state(sys, to, t, &rb, &vb);
	trs_v3 h = v3_norm(v3_cross(ra, va));
	return atan2(v3_dot(h, v3_cross(ra, rb)), v3_dot(ra, rb));
}

int trs_transfer_window(const trs_system *sys, int from, int to, double t, double park_r, trs_transfer *out) {
	if (from <= 0 || to <= 0 || from == to)
		return 0;
	const trs_body *a = &sys->bodies[from], *b = &sys->bodies[to];
	if (a->parent != b->parent || a->parent < 0)
		return 0;
	double mu = sys->bodies[a->parent].mu;
	double r1 = a->orbit.a, r2 = b->orbit.a;
	double at = 0.5 * (r1 + r2);
	out->flight_time = TRS_PI * sqrt(at * at * at / mu);
	double n1 = sqrt(mu / (r1 * r1 * r1)), n2 = sqrt(mu / (r2 * r2 * r2));
	out->phase_best = TRS_PI - n2 * out->flight_time; /* to's lead at departure */
	double ang = lead_at(sys, from, to, t); /* to's lead now */
	out->phase_now = ang;
	/* the lead changes at about (n2 - n1): a first guess at when it equals
	 * the best one, then corrected against the bodies' real positions
	 * (their orbits aren't quite circles) */
	double rel = n2 - n1, d = remainder(out->phase_best - ang, 2 * TRS_PI);
	double wait = d / rel;
	double synodic = 2 * TRS_PI / fabs(rel);
	while (wait < 0)
		wait += synodic;
	while (wait >= synodic)
		wait -= synodic;
	for (int it = 0; it < 30; it++) {
		double err = remainder(lead_at(sys, from, to, t + wait) - out->phase_best, 2 * TRS_PI);
		wait -= err / rel;
		if (fabs(err) < 1e-12)
			break;
	}
	if (wait < 0)
		wait += synodic;
	out->wait = wait;
	out->synodic = synodic;
	/* the speed to leave with (hyperbolic excess), and the burn from a
	 * circular parking orbit of radius park_r around `from` */
	double v_t = sqrt(mu * (2 / r1 - 1 / at)), v_from = sqrt(mu / r1);
	out->v_inf = fabs(v_t - v_from);
	out->dv_eject = park_r > 0 ? sqrt(out->v_inf * out->v_inf + 2 * a->mu / park_r) - sqrt(a->mu / park_r) : 0;
	return 1;
}

/* ---- planning a transfer -------------------------------------------------------- */

/* How good a burn is: a path into `to`'s sphere scores its periapsis error
 * (well below zero); otherwise how far outside the sphere it passes. */
static double plan_score(const trs_system *sys, int body, const trs_orbit *o, double t_now, int to, double want_pe,
		const trs_node *n, double horizon, double *pe_alt) {
	trs_patch p[8];
	int np = trs_trajectory(sys, body, o, t_now, n, 1, horizon, p, 8);
	const trs_body *tb = &sys->bodies[to];
	double best = INFINITY;
	*pe_alt = NAN;
	for (int i = 0; i < np; i++) {
		if (p[i].body == to) {
			*pe_alt = p[i].orbit.rp - tb->radius;
			return -1e12 + fabs(*pe_alt - want_pe);
		}
		if (p[i].body != tb->parent)
			continue;
		double t0 = p[i].t0, t1 = fmin(p[i].t1, t_now + horizon);
		for (int k = 0; k <= 400; k++) {
			double t = t0 + (t1 - t0) * k / 400.0;
			trs_v3 r, v, rb, vb;
			trs_orbit_state_at(&p[i].orbit, t, &r, &v);
			trs_body_state(sys, to, t, &rb, &vb);
			best = fmin(best, v3_len(v3_sub(r, rb)) - tb->soi);
		}
	}
	return best;
}

int trs_plan_transfer(const trs_system *sys, int body, const trs_orbit *o, double t_now, int to, double want_pe_alt,
		trs_node *out, double *pe_alt) {
	trs_transfer tr;
	trs_v3 r0, v0;
	trs_orbit_state_at(o, t_now, &r0, &v0);
	if (!trs_transfer_window(sys, body, to, t_now, v3_len(r0), &tr) || o->period <= 0)
		return 0;
	double horizon = tr.wait + tr.flight_time * 1.6 + o->period;
	double best = INFINITY, bt = 0, bdv = 0, pe;
	/* a grid over one parking orbit around the window, and the burn size
	 * around the Hohmann estimate */
	for (int i = 0; i < 48; i++) {
		double t = t_now + fmax(0, tr.wait - o->period * 0.5) + o->period * i / 48.0;
		for (int j = 0; j < 13; j++) {
			double dv = tr.dv_eject * (0.9 + 0.025 * j);
			trs_node n = { t, dv, 0, 0 };
			double s = plan_score(sys, body, o, t_now, to, want_pe_alt, &n, horizon, &pe);
			if (s < best)
				best = s, bt = t, bdv = dv;
		}
	}
	/* then a pattern search on (time, size), also trying a little
	 * normal burn to match the planes */
	double st = o->period / 48.0, sdv = tr.dv_eject * 0.025, snrm = tr.dv_eject * 0.02, srad = tr.dv_eject * 0.02, bn = 0, br = 0;
	for (int it = 0; it < 600 && (st > 1e-4 || sdv > 1e-5); it++) {
		int moved = 0;
		double cand[8][4] = { { st, 0, 0, 0 }, { -st, 0, 0, 0 }, { 0, sdv, 0, 0 }, { 0, -sdv, 0, 0 }, { 0, 0, snrm, 0 }, { 0, 0, -snrm, 0 },
			{ 0, 0, 0, srad }, { 0, 0, 0, -srad } };
		for (int c = 0; c < 8; c++) {
			trs_node n = { bt + cand[c][0], bdv + cand[c][1], bn + cand[c][2], br + cand[c][3] };
			if (n.t < t_now + 1)
				continue;
			double s = plan_score(sys, body, o, t_now, to, want_pe_alt, &n, horizon, &pe);
			if (s < best) {
				best = s, bt = n.t, bdv = n.pro, bn = n.nrm, br = n.rad;
				moved = 1;
			}
		}
		if (!moved)
			st *= 0.5, sdv *= 0.5, snrm *= 0.5, srad *= 0.5;
		else if (best < -1e11 && fabs(best + 1e12) < 0.25 * sys->bodies[to].soi)
			break; /* well inside: a correction on the way does the fine aiming */
	}
	out->t = bt, out->pro = bdv, out->nrm = bn, out->rad = br;
	trs_node fin = *out;
	plan_score(sys, body, o, t_now, to, want_pe_alt, &fin, horizon, pe_alt);
	return best < -1e11;
}

int trs_plan_correction(const trs_system *sys, int body, const trs_orbit *o, double t_now, int to, double want_pe_alt,
		double horizon, trs_node *out, double *pe_alt) {
	/* a small burn a minute from now, searched by pattern steps in
	 * (prograde, normal, radial) from nothing */
	double t = t_now + 60, b[3] = { 0, 0, 0 }, step = 10, pe;
	trs_node n0 = { t, 0, 0, 0 };
	double best = plan_score(sys, body, o, t_now, to, want_pe_alt, &n0, horizon, &pe);
	for (int it = 0; it < 2000 && step > 1e-6; it++) {
		int moved = 0;
		for (int k = 0; k < 6; k++) {
			double c[3] = { b[0], b[1], b[2] };
			c[k / 2] += (k % 2 ? -step : step);
			trs_node n = { t, c[0], c[1], c[2] };
			double s = plan_score(sys, body, o, t_now, to, want_pe_alt, &n, horizon, &pe);
			if (s < best) {
				best = s, b[0] = c[0], b[1] = c[1], b[2] = c[2];
				moved = 1;
			}
		}
		if (!moved)
			step *= 0.5;
		else if (best < -1e11 && fabs(best + 1e12) < 1000)
			break;
	}
	out->t = t, out->pro = b[0], out->nrm = b[1], out->rad = b[2];
	plan_score(sys, body, o, t_now, to, want_pe_alt, out, horizon, pe_alt);
	return best < -1e11;
}
