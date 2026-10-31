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

/* Lasers, and what they do to things. A laser part's beam starts at its
 * mount and runs straight along where it points, out to its range; the
 * first thing in the way takes its dps: a part of a vessel (each part a
 * capsule along its axis, its own radius round it), an animal (a ball its
 * size), or the ground (which stops it). A turret turns at its slew rate
 * toward what it's told to and reaches only so far off its mount's axis
 * (the hull is in the way past that); it fires only once it's on, and only
 * while the batteries pay for it.
 *
 * Who says where: the pilot by hand (a point to fire at, held down), or a
 * lock (track that thing, fire while it's in reach), or guard mode (pick
 * the nearest threat in reach and see to it: 1 whatever threatens the
 * ship, the Jaegers and hostile animals; 2 anything alive but penguins).
 * The computer won't fire through a penguin or one of our own ships to get
 * at a target; a pilot firing by hand can.
 *
 * A part that has taken its hp breaks (TRS_DESTROY_LASER, or BITTEN for an
 * animal's doing); a penguin on EVA heals once left alone a while. */
#include "trs/world.h"

#include <math.h>
#include <string.h>

#define AIM_ON 0.026       /* rad (1.5 deg): close enough to fire */
#define PICK_EVERY 0.5     /* s between guard mode's looks round */
#define HEAL_AFTER 8.0     /* s untouched before a penguin heals */
#define HEAL_RATE 3.0      /* hp a second */

/* where a vessel is and how it's turned (SOI frame), any mode near a body */
static int pose(const trs_world *w, const trs_vessel *vs, trs_v3 *r, trs_quat *q) {
	const trs_body *b = &w->sys.bodies[vs->body];
	if (vs->mode == TRS_MODE_LANDED) {
		*r = trs_body_from_fixed(b, w->t, vs->fixed_r);
		*q = quat_normalize(quat_mul(trs_body_quat(b, w->t), vs->fixed_q));
		return 1;
	}
	if (vs->mode != TRS_MODE_PHYSICS)
		return 0;
	*r = vs->r;
	*q = vs->q;
	return 1;
}

void trs_part_hurt(trs_world *w, int id, int part, double hp, double code) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive || !w->vessels[id].craft)
		return;
	trs_vessel *vs = &w->vessels[id];
	if (part < 0 || part >= vs->craft->count)
		return;
	trs_part *p = &vs->craft->parts[part];
	p->damage += hp;
	p->hurt_t = w->t;
	if (p->damage < trs_craft_def(vs->craft, part)->hp)
		return;
	if (vs->mode == TRS_MODE_LANDED && !vs->anchored)
		trs_vessel_set_mode(w, id, TRS_MODE_PHYSICS); /* (pieces fly from where it really is) */
	for (int k = 0; k < vs->npending; k++)
		if (vs->pending_uid[k] == p->uid)
			return;
	if (vs->npending < TRS_MAX_PENDING) {
		vs->pending_uid[vs->npending] = p->uid;
		vs->pending_speed[vs->npending++] = code;
	}
}

static int vessel_ok(const trs_world *w, trs_thing th) {
	return th.kind == TRS_THING_VESSEL && th.id >= 0 && th.id < w->nvessels && w->vessels[th.id].alive &&
		w->vessels[th.id].serial == th.serial;
}

int trs_thing_where(const trs_world *w, int body, trs_thing th, trs_v3 *at, double *radius) {
	if (th.kind == TRS_THING_VESSEL) {
		if (!vessel_ok(w, th))
			return 0;
		const trs_vessel *vs = &w->vessels[th.id];
		trs_v3 r;
		trs_quat q;
		if (vs->body != body || !vs->craft || !pose(w, vs, &r, &q))
			return 0;
		*at = r;
		*radius = trs_vessel_is_eva(vs) ? 0.35 : fmax(0.5, vs->bound_radius);
		return 1;
	}
	if (th.kind == TRS_THING_CRITTER) {
		if (w->wild.body != body)
			return 0;
		int i = trs_wild_find(w, th.id);
		if (i < 0 || w->wild.c[i].state == TRS_CRIT_DOWN)
			return 0;
		const trs_critter *c = &w->wild.c[i];
		trs_v3 p = v3_madd(c->pos, v3_norm(c->pos), 0.45 * c->size);
		*at = trs_body_from_fixed(&w->sys.bodies[body], w->t, p);
		*radius = 0.45 * c->size;
		return 1;
	}
	return 0;
}

/* ray o + t d against a ball: the nearest t in [0, range] it enters, or -1 */
static double ray_ball(trs_v3 o, trs_v3 d, trs_v3 c, double r, double range) {
	trs_v3 oc = v3_sub(o, c);
	double b = v3_dot(oc, d), cc = v3_len2(oc) - r * r;
	if (cc <= 0)
		return 0;
	double disc = b * b - cc;
	if (disc < 0)
		return -1;
	double t = -b - sqrt(disc);
	return t >= 0 && t <= range ? t : -1;
}

/* ray against a capsule a..b of radius r (a part along its axis) */
static double ray_capsule(trs_v3 o, trs_v3 d, trs_v3 a, trs_v3 b, double r, double range) {
	trs_v3 u = v3_sub(b, a);
	double L2 = v3_len2(u);
	if (L2 < 1e-12)
		return ray_ball(o, d, a, r, range);
	/* closest points of the line o + t d (d unit) and the segment a + s u */
	trs_v3 w0 = v3_sub(o, a);
	double bb = v3_dot(d, u), dd = v3_dot(d, w0), ee = v3_dot(u, w0);
	double den = L2 - bb * bb, s = den > 1e-12 ? (ee - bb * dd) / den : 0, t;
	s = fmax(0, fmin(1, s));
	trs_v3 q = v3_madd(a, u, s);
	/* with s pinned, the nearest t to that point */
	t = v3_dot(v3_sub(q, o), d);
	if (t < -r)
		return -1; /* (behind it) */
	trs_v3 p = v3_madd(o, d, t);
	double gap2 = v3_len2(v3_sub(p, q));
	if (gap2 > r * r)
		return -1;
	t -= sqrt(r * r - gap2);
	if (t < 0)
		t = 0;
	return t <= range ? t : -1;
}

static double above_ground(const trs_body *b, trs_v3 p) {
	double R = v3_len(p);
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, v3_scale(p, 1.0 / R)) : 0;
	return R - b->radius - h;
}

/* where a ray (body-fixed) first goes under the ground, within range; -1 if not */
static double ray_ground(const trs_body *b, trs_v3 o, trs_v3 d, double range) {
	if (b->gas_giant)
		return -1;
	double step = fmax(3.0, range / 32), t0 = 0, a0 = above_ground(b, o);
	if (a0 < 0) {
		/* (a mount right on the ground: start a little out) */
		t0 = 1.0;
		a0 = above_ground(b, v3_madd(o, d, t0));
		if (a0 < 0)
			return 0;
	}
	for (double t = t0 + step;; t += step) {
		if (t > range)
			t = range;
		double a = above_ground(b, v3_madd(o, d, t));
		if (a < 0) {
			double lo = t - step < t0 ? t0 : t - step, hi = t;
			for (int k = 0; k < 6; k++) {
				double m = 0.5 * (lo + hi);
				if (above_ground(b, v3_madd(o, d, m)) < 0)
					hi = m;
				else
					lo = m;
			}
			return hi;
		}
		if (t >= range)
			return -1;
	}
}

trs_thing trs_world_ray(const trs_world *w, int body, trs_v3 o, trs_v3 d, double range, int skip, double *dist, int *part) {
	trs_thing best = { TRS_THING_NONE, -1, 0 };
	double bt = range;
	int bp = -1;
	for (int i = 0; i < w->nvessels; i++) {
		const trs_vessel *vs = &w->vessels[i];
		trs_v3 r;
		trs_quat q;
		if (i == skip || !vs->alive || !vs->craft || vs->body != body || !pose(w, vs, &r, &q))
			continue;
		if (ray_ball(o, d, r, vs->bound_radius + 2.0, bt) < 0)
			continue;
		for (int k = 0; k < vs->craft->count; k++) {
			const trs_part_def *pd = trs_craft_def(vs->craft, k);
			const trs_part *p = &vs->craft->parts[k];
			trs_v3 c = v3_add(r, quat_rotate(q, v3_sub(p->pos, vs->cm)));
			trs_v3 ax = quat_rotate(q, quat_rotate(p->rot, v3(0, 1, 0)));
			double half = 0.5 * pd->height, rad = fmax(pd->radius, 0.05);
			if (rad > half)
				half = 0; /* (a squat part: a ball) */
			else
				half -= rad;
			double t = ray_capsule(o, d, v3_madd(c, ax, -half), v3_madd(c, ax, half), rad, bt);
			if (t >= 0 && t < bt)
				bt = t, bp = k, best = (trs_thing){ TRS_THING_VESSEL, i, vs->serial };
		}
	}
	if (w->wild.body == body)
		for (int i = 0; i < w->wild.n; i++) {
			const trs_critter *c = &w->wild.c[i];
			if (c->state == TRS_CRIT_DOWN)
				continue;
			trs_thing th = { TRS_THING_CRITTER, c->id, 0 };
			trs_v3 at;
			double rad;
			trs_thing_where(w, body, th, &at, &rad);
			double t = ray_ball(o, d, at, rad, bt);
			if (t >= 0 && t < bt)
				bt = t, bp = -1, best = th;
		}
	const trs_body *b = &w->sys.bodies[body];
	double tg = ray_ground(b, trs_body_to_fixed(b, w->t, o), trs_body_to_fixed(b, w->t, d), bt);
	if (tg >= 0 && tg < bt)
		bt = tg, bp = -1, best = (trs_thing){ TRS_THING_GROUND, -1, 0 };
	if (dist)
		*dist = best.kind ? bt : range;
	if (part)
		*part = bp;
	return best;
}

int trs_vessel_alive_craft(const trs_vessel *vs) {
	/* someone (or something) aboard to fly it: a pod, a probe, a penguin, a
	 * Jaeger's hull -- not wreckage */
	if (!vs->craft)
		return 0;
	for (int i = 0; i < vs->craft->count; i++) {
		const trs_part_def *d = trs_craft_def(vs->craft, i);
		if (d->crew > 0 || d->is_eva || d->alien_pilot || (d->torque > 0 && !d->is_alien))
			return 1;
	}
	return 0;
}

int trs_thing_threat(const trs_world *w, int id, trs_thing th, int mode) {
	const trs_vessel *me = &w->vessels[id];
	if (th.kind == TRS_THING_VESSEL) {
		if (!vessel_ok(w, th) || th.id == id)
			return 0;
		const trs_vessel *o = &w->vessels[th.id];
		return o->is_alien != me->is_alien && !o->anchored && trs_vessel_alive_craft(o); /* (a flag, wreckage: no threat) */
	}
	if (th.kind == TRS_THING_CRITTER) {
		int i = trs_wild_find(w, th.id);
		if (i < 0 || w->wild.c[i].state == TRS_CRIT_DOWN || me->is_alien)
			return 0;
		const trs_critter *c = &w->wild.c[i];
		const trs_life *L = trs_critter_kind(w, c);
		if (mode >= 2)
			return !L->penguin;
		return L->temper == TRS_TEMPER_HOSTILE || c->state == TRS_CRIT_CHARGE;
	}
	return 0;
}

void trs_laser_mount(const trs_world *w, const trs_vessel *vs, int part, trs_v3 *at, trs_v3 *axis) {
	trs_v3 r;
	trs_quat q;
	pose(w, vs, &r, &q);
	const trs_part *p = &vs->craft->parts[part];
	const trs_part_def *d = trs_craft_def(vs->craft, part);
	trs_v3 ax = quat_rotate(p->rot, v3(0, 0, 1));
	trs_v3 local = trs_vessel_is_eva(vs) ? v3_add(p->pos, v3(0, 0.25, 0)) : v3_madd(p->pos, ax, 0.6 * d->height);
	*at = v3_add(r, quat_rotate(q, v3_sub(local, vs->cm)));
	*axis = quat_rotate(q, ax);
}

int trs_world_set_guard(trs_world *w, int id, int mode) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive)
		return 0;
	w->vessels[id].guard = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
	w->vessels[id].guard_on = (trs_thing){ 0, 0, 0 };
	w->vessels[id].guard_pick_t = -1e300;
	return w->vessels[id].nlasers;
}

int trs_world_lock(trs_world *w, int id, trs_thing th) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive)
		return 0;
	w->vessels[id].lock = th;
	return w->vessels[id].nlasers;
}

int trs_world_hand_fire(trs_world *w, int id, int on, trs_v3 at) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive)
		return 0;
	w->vessels[id].hand = on;
	/* (held to the ground: what the pilot points at turns with the body) */
	w->vessels[id].hand_at = trs_body_to_fixed(&w->sys.bodies[w->vessels[id].body], w->t, at);
	return w->vessels[id].nlasers;
}

static double laser_reach(const trs_vessel *vs) {
	double r = 0;
	for (int i = 0; i < vs->craft->count; i++)
		r = fmax(r, trs_craft_def(vs->craft, i)->laser_range);
	return r;
}

/* guard mode's pick: the nearest threat in reach that a beam can get to */
static trs_thing pick(const trs_world *w, int id) {
	const trs_vessel *vs = &w->vessels[id];
	trs_v3 r = v3(0, 0, 0), axis = v3(0, 0, 1);
	double arc = TRS_PI;
	/* (seen from its first laser: the hull's own middle may be down in the grass) */
	for (int i = 0; i < vs->craft->count; i++)
		if (trs_craft_def(vs->craft, i)->laser_range > 0) {
			trs_laser_mount(w, vs, i, &r, &axis);
			arc = trs_craft_def(vs->craft, i)->laser_arc;
			break;
		}
	double reach = laser_reach(vs), bd = reach;
	trs_thing best = { TRS_THING_NONE, -1, 0 };
	trs_thing cand[TRS_WILD_MAX + TRS_MAX_VESSELS];
	int nc = 0;
	for (int i = 0; i < w->nvessels; i++)
		if (w->vessels[i].alive && i != id && w->vessels[i].body == vs->body)
			cand[nc++] = (trs_thing){ TRS_THING_VESSEL, i, w->vessels[i].serial };
	if (w->wild.body == vs->body)
		for (int i = 0; i < w->wild.n; i++)
			cand[nc++] = (trs_thing){ TRS_THING_CRITTER, w->wild.c[i].id, 0 };
	for (int k = 0; k < nc; k++) {
		trs_v3 at;
		double rad;
		if (!trs_thing_threat(w, id, cand[k], vs->guard) || !trs_thing_where(w, vs->body, cand[k], &at, &rad))
			continue;
		double d = v3_len(v3_sub(at, r)) - rad;
		if (d >= bd)
			continue;
		/* in sight of the ship (the ground not in the way) */
		trs_v3 dir = v3_norm(v3_sub(at, r));
		if (arc < TRS_PI && acos(fmax(-1, fmin(1, v3_dot(dir, axis)))) > arc)
			continue; /* (where it can't turn: under the hull) */
		const trs_body *b = &w->sys.bodies[vs->body];
		double tg = ray_ground(b, trs_body_to_fixed(b, w->t, r), trs_body_to_fixed(b, w->t, dir), fmax(0, d));
		if (tg >= 0)
			continue;
		bd = d, best = cand[k];
	}
	return best;
}

/* turns unit vector a toward unit vector b by at most `most` rad */
static trs_v3 turn_toward(trs_v3 a, trs_v3 b, double most) {
	double c = fmax(-1, fmin(1, v3_dot(a, b))), ang = acos(c);
	if (ang <= most)
		return b;
	trs_v3 perp = v3_madd(b, a, -c);
	double pl = v3_len(perp);
	if (pl < 1e-9) {
		perp = v3_cross(a, fabs(a.x) < 0.9 ? v3(1, 0, 0) : v3(0, 1, 0));
		pl = v3_len(perp);
	}
	perp = v3_scale(perp, 1.0 / pl);
	return v3_norm(v3_add(v3_scale(a, cos(most)), v3_scale(perp, sin(most))));
}

static void lasers(trs_world *w, int id, double h) {
	trs_vessel *vs = &w->vessels[id];
	trs_v3 r;
	trs_quat q;
	for (int i = 0; i < vs->craft->count; i++)
		vs->craft->parts[i].beam = 0, vs->craft->parts[i].beam_hit = 0;
	if (!pose(w, vs, &r, &q))
		return;
	/* what to fire at */
	trs_v3 point = v3(0, 0, 0);
	int have = 0, computer = 0;
	trs_thing target = { 0, 0, 0 };
	if (vs->hand) {
		point = trs_body_from_fixed(&w->sys.bodies[vs->body], w->t, vs->hand_at), have = 1;
	} else {
		double rad;
		if (vs->lock.kind && !trs_thing_where(w, vs->body, vs->lock, &point, &rad)) {
			if (vs->lock.kind == TRS_THING_CRITTER || !vessel_ok(w, vs->lock))
				vs->lock = (trs_thing){ 0, 0, 0 }; /* gone for good */
		} else if (vs->lock.kind)
			have = 1, computer = 1, target = vs->lock;
		if (!have && vs->guard) {
			if (w->t - vs->guard_pick_t >= PICK_EVERY || !trs_thing_threat(w, id, vs->guard_on, vs->guard)) {
				vs->guard_on = pick(w, id);
				vs->guard_pick_t = w->t;
			}
			if (vs->guard_on.kind && trs_thing_where(w, vs->body, vs->guard_on, &point, &rad))
				have = 1, computer = 1, target = vs->guard_on;
		}
	}
	if (!have)
		return;
	trs_quat qi = quat_conj(q);
	for (int i = 0; i < vs->craft->count; i++) {
		trs_part *p = &vs->craft->parts[i];
		const trs_part_def *d = trs_craft_def(vs->craft, i);
		if (d->laser_range <= 0)
			continue;
		trs_v3 at, axis;
		trs_laser_mount(w, vs, i, &at, &axis);
		trs_v3 to = v3_sub(point, at);
		double dist = v3_len(to);
		if (dist < 1e-6)
			continue;
		trs_v3 want = v3_scale(to, 1.0 / dist);
		/* past its reach off the mount, it turns as far as it can and holds */
		int bears = d->laser_arc >= TRS_PI || acos(fmax(-1, fmin(1, v3_dot(want, axis)))) <= d->laser_arc;
		if (!bears)
			want = turn_toward(axis, want, d->laser_arc);
		trs_v3 aim = v3_len2(p->aim) > 0.5 ? quat_rotate(q, p->aim) : axis;
		aim = turn_toward(v3_norm(aim), want, d->laser_slew * h);
		p->aim = quat_rotate(qi, aim);
		if (!bears || acos(fmax(-1, fmin(1, v3_dot(aim, want)))) > AIM_ON)
			continue;
		if (computer && dist > d->laser_range + 2.0)
			continue; /* (out of reach: the computer waits) */
		double kj = d->laser_kw * h;
		if (kj > 0 && trs_craft_ec(vs->craft, NULL) < kj)
			continue;
		double len;
		int hp;
		trs_thing hit = trs_world_ray(w, vs->body, at, aim, d->laser_range, id, &len, &hp);
		if (computer && hit.kind != TRS_THING_GROUND && hit.kind != TRS_THING_NONE &&
				!(hit.kind == target.kind && hit.id == target.id) && !trs_thing_threat(w, id, hit, 2))
			continue; /* one of ours in the way: the computer holds fire */
		if (kj > 0) {
			trs_craft_ec_add(vs->craft, -kj);
			vs->ec_use += d->laser_kw;
		}
		p->beam = len;
		p->beam_hit = hit.kind;
		double dmg = d->laser_dps * h;
		trs_thing me = { TRS_THING_VESSEL, id, vs->serial };
		if (hit.kind == TRS_THING_VESSEL)
			trs_part_hurt(w, hit.id, hp, dmg, TRS_DESTROY_LASER);
		else if (hit.kind == TRS_THING_CRITTER) {
			int ci = trs_wild_find(w, hit.id);
			if (ci >= 0)
				trs_critter_hurt(w, ci, dmg, me);
		}
	}
}

void trs_world_lasers_step(trs_world *w, double h) {
	for (int id = 0; id < w->nvessels; id++) {
		trs_vessel *vs = &w->vessels[id];
		if (!vs->alive || !vs->craft)
			continue;
		if (vs->nlasers)
			lasers(w, id, h);
		/* a penguin left alone a while heals */
		if (trs_vessel_is_eva(vs)) {
			trs_part *p = &vs->craft->parts[0];
			if (p->damage > 0 && w->t - p->hurt_t > HEAL_AFTER)
				p->damage = fmax(0, p->damage - HEAL_RATE * h);
		}
	}
}

int trs_laser_reaches(const trs_world *w, int id, trs_thing th) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive || !w->vessels[id].craft)
		return 0;
	const trs_vessel *vs = &w->vessels[id];
	trs_v3 point;
	double rad;
	if (!trs_thing_where(w, vs->body, th, &point, &rad))
		return 0;
	for (int i = 0; i < vs->craft->count; i++) {
		const trs_part_def *d = trs_craft_def(vs->craft, i);
		if (d->laser_range <= 0)
			continue;
		trs_v3 at, axis;
		trs_laser_mount(w, vs, i, &at, &axis);
		trs_v3 to = v3_sub(point, at);
		double dist = v3_len(to);
		if (dist < 1e-6 || dist > d->laser_range + 2.0)
			continue;
		trs_v3 dir = v3_scale(to, 1.0 / dist);
		if (d->laser_arc < TRS_PI && acos(fmax(-1, fmin(1, v3_dot(dir, axis)))) > d->laser_arc)
			continue;
		double len;
		int hp;
		trs_thing hit = trs_world_ray(w, vs->body, at, dir, d->laser_range, id, &len, &hp);
		if (hit.kind == th.kind && hit.id == th.id)
			return 1;
	}
	return 0;
}
