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

/* Contact between vessels: a dropped capsule lands on the stage below it
 * instead of falling through, debris bounces off the rocket it came from.
 *
 * Points sampled over each vessel's hull parts are tested against the
 * other vessel's parts as solid cylinders. A point
 * inside is pushed out through the nearest face by a spring-damper with
 * Coulomb friction, sized from the pair's reduced mass like the ground
 * contact, and the opposite force goes on the other vessel at the same
 * point, so linear and angular momentum are conserved exactly. A vessel
 * held to the surface (launch clamps, landed) is immovable. Forces are
 * evaluated once per tick from the start-of-tick state and held over it.
 * Contact faster than a part's crash tolerance breaks it. */
#include "trs/world.h"

#include <math.h>
#include <string.h>

#define VC_OMEGA 22.0  /* rad/s, as the ground contact */
#define VC_ZETA 0.8
#define VC_MU 0.5
#define VC_SLIP 0.08
#define VC_MAX_DEPTH 0.15 /* m: caps the push from parts that overlapped by design */

typedef struct {
	trs_vessel *vs;
	trs_v3 r, v, w;
	trs_quat q;
	int movable;
} body_state;

static void state_of(trs_world *w, int id, body_state *s) {
	trs_vessel *vs = &w->vessels[id];
	s->vs = vs;
	trs_vessel_state(w, id, &s->r, &s->v);
	if (vs->mode == TRS_MODE_LANDED) {
		const trs_body *b = &w->sys.bodies[vs->body];
		s->q = quat_normalize(quat_mul(trs_body_quat(b, w->t), vs->fixed_q));
		s->w = trs_body_omega(b);
		s->movable = 0;
	} else {
		s->q = vs->q;
		s->w = vs->w;
		s->movable = 1;
	}
}

/* Is world point p inside a hull part of b? Fills the outward normal
 * (world) and depth, and the part index. Fins, legs and chutes are left out:
 * they are thin or flexible, and the ground already handles legs.
 *
 * The push goes out through the nearest face of b's cylinder that faces
 * the incoming surface: na1/na2 are the outward normals of a's hull at the
 * point (an edge point has two), and a face counts only if its normal
 * opposes one of them. That keeps a capsule sitting on an equally wide
 * stage from being shoved sideways off its rim, and two tanks touching
 * side by side from being pushed along their axes. */
static int inside(const body_state *b, trs_v3 p, trs_v3 na1, trs_v3 na2, trs_v3 *n, double *depth, int *part) {
	const trs_craft *c = b->vs->craft;
	trs_v3 lv = v3_add(quat_rotate(quat_conj(b->q), v3_sub(p, b->r)), b->vs->cm);
	double best = 1e30, any = 1e30;
	int hit = -1, hit_any = -1;
	trs_v3 nl = v3(0, 0, 0), nl_any = v3(0, 0, 0);
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		if (d->is_leg || d->is_parachute || d->plate || d->radius <= 0.05)
			continue;
		const trs_part *pt = &c->parts[i];
		trs_v3 lp = quat_rotate(quat_conj(pt->rot), v3_sub(lv, pt->pos));
		double hh = d->height * 0.5, rho = sqrt(lp.x * lp.x + lp.z * lp.z);
		double dy = hh - fabs(lp.y), dr = d->radius - rho;
		if (dy <= 0 || dr <= 0)
			continue;
		trs_v3 faces[2];
		double depths[2];
		int nf = 0;
		faces[nf] = v3(0, lp.y >= 0 ? 1 : -1, 0);
		depths[nf++] = dy;
		if (rho > 1e-9) {
			faces[nf] = v3(lp.x / rho, 0, lp.z / rho);
			depths[nf++] = dr;
		}
		for (int f = 0; f < nf; f++) {
			trs_v3 fw = quat_rotate(b->q, quat_rotate(pt->rot, faces[f]));
			if (depths[f] < any) {
				any = depths[f];
				hit_any = i;
				nl_any = fw;
			}
			int faces_it = v3_dot(fw, na1) < -0.3 || v3_dot(fw, na2) < -0.3;
			if (faces_it && depths[f] < best) {
				best = depths[f];
				hit = i;
				nl = fw;
			}
		}
	}
	if (hit < 0) {
		if (hit_any < 0)
			return 0;
		hit = hit_any, best = any, nl = nl_any;
	}
	*n = nl;
	*depth = best;
	*part = hit;
	return 1;
}

static void queue_crash(trs_vessel *vs, int part, double speed) {
	int uid = vs->craft->parts[part].uid;
	for (int k = 0; k < vs->npending; k++)
		if (vs->pending_uid[k] == uid) {
			if (vs->pending_speed[k] >= 0 && speed > vs->pending_speed[k])
				vs->pending_speed[k] = speed;
			return;
		}
	if (vs->npending < TRS_MAX_PENDING) {
		vs->pending_uid[vs->npending] = uid;
		vs->pending_speed[vs->npending++] = speed;
	}
}

/* Hull sample points of a part, part-local, with the hull's outward
 * normals there (n2 zero where there is only one): rings of 8 just inside
 * each end and at the middle, and the two end centres. Inset a hair so two
 * parts with flush ends still register. */
static int part_points(const trs_part_def *d, trs_v3 *out, trs_v3 *n1, trs_v3 *n2) {
	if (d->is_leg || d->is_parachute || d->plate || d->radius <= 0.05)
		return 0;
	int m = 0;
	double r = d->radius * 0.999, hh = d->height * 0.5;
	double ys[3] = { -hh * 0.98, 0, hh * 0.98 };
	for (int j = 0; j < 3; j++)
		for (int k = 0; k < 8; k++) {
			double t = TRS_TAU * k / 8;
			n1[m] = v3(sin(t), 0, cos(t));
			n2[m] = j == 1 ? v3(0, 0, 0) : v3(0, j == 0 ? -1 : 1, 0);
			out[m++] = v3(r * sin(t), ys[j], r * cos(t));
		}
	n1[m] = v3(0, -1, 0), n2[m] = v3(0, 0, 0);
	out[m++] = v3(0, -hh, 0);
	n1[m] = v3(0, 1, 0), n2[m] = v3(0, 0, 0);
	out[m++] = v3(0, hh, 0);
	return m;
}

typedef struct {
	body_state *a, *b; /* a's point p is inside b's part bp */
	int ap, bp;
	trs_v3 p, n;
	double depth;
} contact;

#define VC_MAX_CONTACTS 512

/* Points of a pushing into b, appended to cs. */
static int one_way(body_state *a, body_state *b, contact *cs, int n) {
	const trs_craft *ca = a->vs->craft;
	double reach = b->vs->bound_radius + 0.5;
	trs_v3 pts[32], n1[32], n2[32];
	for (int ap = 0; ap < ca->count; ap++) {
		const trs_part *pa = &ca->parts[ap];
		int np = part_points(trs_craft_def(ca, ap), pts, n1, n2);
		for (int i = 0; i < np && n < VC_MAX_CONTACTS; i++) {
			trs_v3 local = v3_add(pa->pos, quat_rotate(pa->rot, pts[i]));
			trs_v3 p = v3_add(a->r, quat_rotate(a->q, v3_sub(local, a->vs->cm)));
			if (v3_len(v3_sub(p, b->r)) > reach)
				continue;
			contact *ct = &cs[n];
			trs_quat qa = quat_mul(a->q, pa->rot);
			if (!inside(b, p, quat_rotate(qa, n1[i]), quat_rotate(qa, n2[i]), &ct->n, &ct->depth, &ct->bp))
				continue;
			ct->a = a;
			ct->b = b;
			ct->ap = ap;
			ct->p = p;
			n++;
		}
	}
	return n;
}

/* Spring-damper push for each contact; the pair's stiffness is shared out
 * over the contacts, so a flat face resting on another (many points) is no
 * stiffer than the ground holds a hull, and stays stable at 50 Hz. */
static void apply(contact *cs, int n, double mu) {
	double share = n > 4 ? 4.0 / n : 1.0;
	double k = mu * VC_OMEGA * VC_OMEGA / 4.0 * share;
	double c = 2 * VC_ZETA * sqrt(k * mu / 4.0 * share);
	for (int i = 0; i < n; i++) {
		contact *ct = &cs[i];
		body_state *a = ct->a, *b = ct->b;
		trs_v3 arm_a = v3_sub(ct->p, a->r), arm_b = v3_sub(ct->p, b->r);
		trs_v3 rel = v3_sub(v3_add(a->v, v3_cross(a->w, arm_a)), v3_add(b->v, v3_cross(b->w, arm_b)));
		double vn = v3_dot(rel, ct->n);
		double fn = k * fmin(ct->depth, VC_MAX_DEPTH) - c * vn;
		if (fn < 0)
			fn = 0;
		trs_v3 vt = v3_sub(rel, v3_scale(ct->n, vn));
		double vtl = v3_len(vt);
		/* Coulomb, but never more than would stop the slip within the tick
		 * (held over the tick, a stiffer push would overshoot and pump) */
		double ft = fmin(VC_MU * fn * fmin(vtl / VC_SLIP, 1.0), 0.5 * mu * vtl / (TRS_PHYSICS_DT * n));
		trs_v3 f = v3_add(v3_scale(ct->n, fn), vtl > 1e-12 ? v3_scale(vt, -ft / vtl) : v3(0, 0, 0));
		if (a->movable) {
			a->vs->ext_force = v3_add(a->vs->ext_force, f);
			a->vs->ext_torque = v3_add(a->vs->ext_torque, quat_rotate(quat_conj(a->q), v3_cross(arm_a, f)));
		}
		if (b->movable) {
			b->vs->ext_force = v3_sub(b->vs->ext_force, f);
			b->vs->ext_torque = v3_sub(b->vs->ext_torque, quat_rotate(quat_conj(b->q), v3_cross(arm_b, f)));
		}
		/* closing faster than either part can take: the weaker one breaks */
		double impact = -vn;
		double ta = trs_craft_def(a->vs->craft, ct->ap)->crash_tolerance;
		double tb = trs_craft_def(b->vs->craft, ct->bp)->crash_tolerance;
		if (impact > fmin(ta, tb)) {
			if (ta <= tb)
				queue_crash(a->vs, ct->ap, impact);
			else
				queue_crash(b->vs, ct->bp, impact);
		}
	}
}

void trs_world_vessel_contacts(trs_world *w) {
	static contact cs[VC_MAX_CONTACTS];
	for (int i = 0; i < w->nvessels; i++) {
		w->vessels[i].ext_force = v3(0, 0, 0);
		w->vessels[i].ext_torque = v3(0, 0, 0);
	}
	for (int i = 0; i < w->nvessels; i++) {
		trs_vessel *x = &w->vessels[i];
		if (!x->alive || !x->craft || x->mode == TRS_MODE_RAILS)
			continue;
		for (int j = i + 1; j < w->nvessels; j++) {
			trs_vessel *y = &w->vessels[j];
			if (!y->alive || !y->craft || y->mode == TRS_MODE_RAILS || y->body != x->body)
				continue;
			if (x->mode != TRS_MODE_PHYSICS && y->mode != TRS_MODE_PHYSICS)
				continue;
			if (x->dirty)
				trs_vessel_refresh(w, i);
			if (y->dirty)
				trs_vessel_refresh(w, j);
			body_state a, b;
			state_of(w, i, &a);
			state_of(w, j, &b);
			if (v3_len(v3_sub(a.r, b.r)) > x->bound_radius + y->bound_radius + 1.0)
				continue;
			/* stiffness from the reduced mass (an immovable side counts as infinite) */
			double mu = a.movable && b.movable ? x->mass * y->mass / (x->mass + y->mass) : a.movable ? x->mass : y->mass;
			int n = one_way(&a, &b, cs, 0);
			n = one_way(&b, &a, cs, n);
			apply(cs, n, mu);
		}
	}
}
