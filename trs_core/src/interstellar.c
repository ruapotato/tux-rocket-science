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

/* Between the stars. Past a star's heliopause a vessel flies by special
 * relativity in the stars' shared rest frame (gravity out there is too
 * weak to matter and is left out): its position and proper velocity are
 * kept relative to the star it left, and its own clock falls behind the
 * home calendar by tau_lag. Inside another star's heliopause it is back
 * in ordinary flight around that star. The Stillwater Drive works only
 * out here. */
#include "trs/relativity.h"
#include "trs/world.h"

#include <math.h>

double trs_vessel_drive_max(const trs_vessel *vs, double *kw) {
	double a = 0, k = 0;
	if (vs->craft)
		for (int i = 0; i < vs->craft->count; i++) {
			const trs_part_def *d = trs_craft_def(vs->craft, i);
			a += d->drive_accel; /* it moves the whole ship, whatever it weighs */
			k += d->drive_kw;
		}
	if (kw)
		*kw = k;
	return a;
}

int trs_world_set_drive(trs_world *w, int id, double throttle) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive)
		return 0;
	trs_vessel *vs = &w->vessels[id];
	int n = 0;
	if (vs->craft)
		for (int i = 0; i < vs->craft->count; i++)
			n += trs_craft_def(vs->craft, i)->drive_accel > 0;
	vs->drive = n ? fmin(1, fmax(0, throttle)) : 0;
	return n;
}

/* Out past a star's heliopause, or in through another's. */
void trs_heliopause_check(trs_world *w, trs_vessel *vs) {
	const trs_system *sys = &w->sys;
	int id = (int)(vs - w->vessels);
	if (vs->mode == TRS_MODE_INTERSTELLAR) {
		trs_v3 here = v3_add(vs->r, sys->bodies[vs->body].galactic);
		for (int s = 0; s < sys->count; s++) {
			const trs_body *st = &sys->bodies[s];
			if (st->parent >= 0 || !isfinite(st->soi))
				continue;
			trs_v3 rel = v3_sub(here, st->galactic);
			if (v3_len(rel) < st->soi * TRS_SOI_ENTER_FACTOR) {
				vs->r = rel;
				vs->v = trs_rel_velocity(vs->u);
				vs->body = s;
				vs->mode = TRS_MODE_PHYSICS;
				vs->drive_accel = 0;
				vs->soi_changes++;
				trs_world_push_event(w, (trs_world_event){ TRS_WEV_SOI_CHANGE, w->t, id, s, -1, "", 0 });
				return;
			}
		}
		return;
	}
	const trs_body *b = &sys->bodies[vs->body];
	if (b->parent >= 0 || !isfinite(b->soi) || vs->mode == TRS_MODE_LANDED)
		return;
	trs_v3 r, v;
	trs_vessel_state(w, id, &r, &v);
	if (v3_len(r) <= b->soi * TRS_SOI_EXIT_FACTOR)
		return;
	vs->r = r;
	vs->v = v;
	vs->u = trs_rel_proper_velocity(v);
	vs->mode = TRS_MODE_INTERSTELLAR;
	trs_world_push_event(w, (trs_world_event){ TRS_WEV_INTERSTELLAR, w->t, id, vs->body, -1, "", 0 });
}

/* the drive's push now, as far as power allows over dt (draws the batteries) */
static trs_v3 drive_push(trs_world *w, trs_vessel *vs, double dt) {
	double kw, amax = trs_vessel_drive_max(vs, &kw);
	vs->drive_accel = 0;
	if (amax <= 0 || vs->drive <= 0)
		return v3(0, 0, 0);
	int lit;
	double gen = trs_vessel_solar(w, vs, &lit), draw = 0;
	for (int i = 0; i < vs->craft->count; i++)
		draw += trs_craft_def(vs->craft, i)->ec_draw;
	double need = kw * vs->drive;
	double have = trs_craft_ec(vs->craft, NULL) / dt + gen - draw;
	double f = need > 0 ? fmin(1, fmax(0, have / need)) : 1;
	vs->drive_accel = amax * vs->drive * f;
	vs->ec_gen = gen;
	vs->ec_use = draw + need * f;
	trs_craft_ec_add(vs->craft, (gen - vs->ec_use) * dt);
	trs_quat q = vs->q;
	return v3_scale(quat_rotate(q, v3(0, 1, 0)), vs->drive_accel);
}

/* on a course: nose along the line (then against it), drive flat out */
static void course_steer(trs_vessel *vs) {
	if (!vs->course)
		return;
	trs_v3 d = vs->course_acc > 0 ? vs->course_dir : v3_neg(vs->course_dir);
	vs->q = quat_normalize(quat_from_to(v3(0, 1, 0), d));
	vs->w = v3(0, 0, 0);
	vs->drive = 1.0;
}

/* ship time spent on the course: speeding up, then slowing, then done */
static void course_spend(trs_vessel *vs, double dtau) {
	if (!vs->course)
		return;
	double use = fmin(dtau, vs->course_acc);
	vs->course_acc -= use;
	dtau -= use;
	if (vs->course_acc <= 1e-9 && dtau > 0) {
		vs->course_acc = 0;
		vs->course_dec -= dtau;
	}
	if (vs->course_acc <= 1e-9 && vs->course_dec <= 1e-9) { /* arrived: at rest, drive off */
		vs->course = 0;
		vs->course_acc = vs->course_dec = 0;
		vs->drive = 0;
	}
}

/* ship time to the course's next turn (INFINITY with none) */
static double course_next(const trs_vessel *vs) {
	if (!vs->course)
		return INFINITY;
	return vs->course_acc > 0 ? vs->course_acc : vs->course_dec;
}

void trs_interstellar_advance(trs_world *w, trs_vessel *vs, double dt) {
	double left = dt;
	for (int guard = 0; guard < 8 && left > 0; guard++) {
		course_steer(vs);
		trs_v3 a = vs->craft ? drive_push(w, vs, left) : v3(0, 0, 0);
		trs_rel_state s = { vs->r, vs->u, 0, 0 };
		trs_rel_state keep = s;
		trs_rel_advance_t(&s, a, left);
		double turn = course_next(vs);
		if (s.tau > turn) { /* the course turns within this step: stop there exactly */
			s = keep;
			trs_rel_advance_tau(&s, a, turn);
		}
		vs->r = s.x;
		vs->u = s.u;
		vs->v = trs_rel_velocity(s.u);
		vs->tau_lag += s.t - s.tau;
		left -= s.t;
		course_spend(vs, s.tau);
		if (left < 1e-9 * dt)
			break;
	}
	vs->sensed_accel = v3(0, 0, 0); /* the drive pulls every atom alike */
}

int trs_world_set_course(trs_world *w, int id, int star, double *tau, double *t) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive)
		return 0;
	trs_vessel *vs = &w->vessels[id];
	if (star < 0) {
		vs->course = 0;
		vs->drive = 0;
		return 1;
	}
	double a = trs_vessel_drive_max(vs, NULL);
	const trs_body *st = &w->sys.bodies[star];
	if (vs->mode != TRS_MODE_INTERSTELLAR || a <= 0 || st->parent >= 0 || !isfinite(st->soi))
		return 0;
	trs_v3 here = v3_add(vs->r, w->sys.bodies[vs->body].galactic);
	trs_v3 dir = v3_norm(v3_sub(st->galactic, here));
	double d = v3_len(v3_sub(st->galactic, here)) - 1.05 * st->soi;
	if (d <= 0)
		return 0;
	/* 1D along the line: from rapidity e0 up to e1, then down to rest */
	double c = TRS_C, e0 = asinh(v3_dot(vs->u, dir) / c);
	double e1 = acosh(fmax(1.0, 0.5 * (d * a / (c * c) + cosh(e0) + 1.0)));
	vs->course = star + 1;
	vs->course_dir = dir;
	vs->course_acc = c / a * (e1 - e0);
	vs->course_dec = c / a * e1;
	vs->drive = 1.0;
	if (tau)
		*tau = vs->course_acc + vs->course_dec;
	if (t)
		*t = c / a * (sinh(e1) - sinh(e0)) + c / a * sinh(e1);
	return 1;
}

double trs_world_warp_ship(trs_world *w, int id, double dtau) {
	trs_vessel *vs = &w->vessels[id];
	double total = 0;
	for (int guard = 0; guard < 16 && dtau > 1e-9 && vs->alive && vs->mode == TRS_MODE_INTERSTELLAR; guard++) {
		double step = dtau;
		if (vs->course) { /* to the course's next turn exactly, then on */
			double next = course_next(vs);
			if (next < 1e-6) {
				course_spend(vs, next);
				if (!vs->course)
					break; /* arrived: warp ends here */
				continue;
			}
			step = fmin(step, next);
		}
		course_steer(vs);
		/* the home time this much of the ship's clock takes, flown on a copy */
		double push = vs->drive_accel > 0 ? vs->drive_accel : trs_vessel_drive_max(vs, NULL) * vs->drive;
		trs_v3 a = v3_scale(quat_rotate(vs->q, v3(0, 1, 0)), push);
		trs_rel_state s = { vs->r, vs->u, 0, 0 };
		trs_rel_advance_tau(&s, a, step);
		trs_event ev;
		trs_world_warp(w, s.t, -1, &ev);
		total += s.t;
		dtau -= step;
	}
	return total;
}
