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

/* The Jaegers: scavengers who strip what the builders left, and who don't
 * like company. A nest (body data "nests") keeps a few raiders hanging
 * over the ground; when the player comes near they go up, and they come
 * down again when the player has gone. Each raider is a real ship (its
 * craft from data/craft), flown here: on its beat it circles the nest; a
 * ship that isn't theirs within `detect` m of it or of the nest, it goes
 * for, holding off a few hundred metres up and round it while its lasers
 * (on guard, locked on) do the rest. It flies on a lift drive, which
 * pushes it any way it likes; take out its pilot's hull and it is wreckage
 * that falls like any other. The nest makes up a loss in `respawn` s.
 * A drill digging within `hear` m of a nest wakes it from wherever the
 * player is, and its raiders come for the noise. */
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEST_CHECK 1.0     /* s between the nests' looks at where the player is */
#define WAKE_MIN 15000.0   /* m: raiders go up when the player is this near a nest (or 3 x its detect) */
#define SLEEP_MIN 25000.0  /* ... and come down past this (or 5 x) */
#define PICK_EVERY 1.0
#define STANDOFF 350.0     /* m from what it attacks, round it */
#define STANDOFF_UP 150.0  /* ... and above it */
#define FLOOR_AGL 60.0     /* never lower than this over the ground */
#define PATROL_SPEED 30.0
#define ATTACK_SPEED 60.0
#define CRUISE_SPEED 200.0 /* m/s on the way to a far fight (a drill heard from the nest) */
#define BURST_ON 2.0       /* s its lasers fire for, then */
#define BURST_OFF 2.0      /* ... s they rest */

trs_v3 trs_nest_home(const trs_body *b, int k) {
	const trs_nest *n = &b->nests[k];
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, n->dir) : 0;
	return v3_scale(n->dir, b->radius + fmax(h, 0) + n->height);
}

void trs_world_set_nest_craft(trs_world *w, const char *craft_id, const trs_craft *c) {
	int k = 0;
	while (k < w->nnest_crafts && strcmp(w->nest_craft_id[k], craft_id))
		k++;
	if (k >= 8)
		return;
	if (k == w->nnest_crafts) {
		w->nnest_crafts++;
		snprintf(w->nest_craft_id[k], sizeof w->nest_craft_id[k], "%s", craft_id);
		w->nest_craft[k] = NULL;
	}
	if (!w->nest_craft[k])
		w->nest_craft[k] = calloc(1, sizeof(trs_craft));
	else
		trs_craft_free(w->nest_craft[k]);
	trs_craft_copy(w->nest_craft[k], c);
}

static const trs_craft *nest_craft(const trs_world *w, const char *id) {
	for (int k = 0; k < w->nnest_crafts; k++)
		if (!strcmp(w->nest_craft_id[k], id))
			return w->nest_craft[k];
	return NULL;
}

/* a vessel's place on its body, body-fixed (0: not on the ground or flying low) */
static int fixed_at(const trs_world *w, const trs_vessel *o, trs_v3 *p) {
	const trs_body *b = &w->sys.bodies[o->body];
	if (o->mode == TRS_MODE_LANDED)
		*p = o->fixed_r;
	else if (o->mode == TRS_MODE_PHYSICS)
		*p = trs_body_to_fixed(b, w->t, o->r);
	else
		return 0;
	return 1;
}

/* a drill on this body digging within the nest's hearing (-1: none heard) */
static int heard_drill(const trs_world *w, int bi, trs_v3 home, double hear) {
	if (hear <= 0)
		return -1;
	int best = -1;
	double bd = 1e300;
	for (int i = 0; i < w->nvessels; i++) {
		const trs_vessel *o = &w->vessels[i];
		trs_v3 p;
		if (!o->alive || o->body != bi || o->is_alien || trs_vessel_noise(w, o) <= 0 || !fixed_at(w, o, &p))
			continue;
		double d = v3_len(v3_sub(p, home));
		if (d < hear && d < bd)
			bd = d, best = i;
	}
	return best;
}

/* does it still have a pilot: the hull its pilot sits in */
static int raiding(const trs_world *w, int code);
static int crewed(const trs_vessel *vs) {
	for (int i = 0; i < vs->craft->count; i++)
		if (trs_craft_def(vs->craft, i)->alien_pilot)
			return 1;
	return 0;
}

int trs_world_recall_aliens(trs_world *w) {
	int n = 0;
	for (int i = 0; i < w->nvessels; i++)
		if (w->vessels[i].alive && w->vessels[i].pilot) {
			trs_world_remove_vessel(w, i);
			n++;
		}
	return n;
}

static void spawn(trs_world *w, int bi, int k, int code, int slot, int of) {
	const trs_body *b = &w->sys.bodies[bi];
	const trs_nest *ne = &b->nests[k];
	const trs_craft *c = nest_craft(w, ne->craft);
	if (!c)
		return;
	trs_v3 H = trs_nest_home(b, k), up, n, e;
	trs_local_frame(H, &up, &n, &e);
	double a = 2 * TRS_PI * slot / (of > 0 ? of : 1);
	trs_v3 at = v3_add(H, v3_scale(v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a))), 0.6 * ne->patrol));
	trs_v3 r = trs_body_from_fixed(b, w->t, at);
	trs_v3 u = v3_norm(r);
	int id = trs_world_add_craft(w, c, bi, r, v3_cross(trs_body_omega(b), r), quat_from_to(v3(0, 1, 0), u));
	if (id < 0)
		return;
	trs_vessel *vs = &w->vessels[id];
	vs->w = trs_body_omega(b);
	vs->pilot = code;
	vs->pilot_phase = a;
	vs->sas = TRS_SAS_UPRIGHT;
	vs->guard = 1;
	vs->lifted = 1;
}

void trs_alien_downed(trs_world *w, int id) {
	trs_vessel *vs = &w->vessels[id];
	if (!vs->pilot)
		return;
	int bi = (vs->pilot - 1) / TRS_MAX_NESTS, k = (vs->pilot - 1) % TRS_MAX_NESTS;
	if (w->nest_lost[bi][k]++ == 0)
		w->nest_lost_t[bi][k] = w->t;
	trs_world_push_event(w, (trs_world_event){ TRS_WEV_DOWNED, w->t, id, bi, -1, "", 0 });
	vs->pilot = 0, vs->guard = 0, vs->lock = (trs_thing){ 0, 0, 0 };
	vs->lift = v3(0, 0, 0), vs->sas = TRS_SAS_OFF;
}

void trs_world_nests_step(trs_world *w, double h) {
	(void)h;
	/* wreckage: a raider whose pilot's hull is gone flies no more */
	for (int i = 0; i < w->nvessels; i++) {
		trs_vessel *vs = &w->vessels[i];
		if (vs->alive && vs->pilot && vs->craft && !crewed(vs))
			trs_alien_downed(w, i);
	}
	if (w->t - w->nest_check_t < NEST_CHECK)
		return;
	w->nest_check_t = w->t;
	const trs_vessel *f = w->focus >= 0 && w->focus < w->nvessels ? &w->vessels[w->focus] : NULL;
	int fb = -1;
	trs_v3 pf = v3(0, 0, 0);
	if (f && f->alive && f->serial == w->focus_serial && (f->mode == TRS_MODE_PHYSICS || f->mode == TRS_MODE_LANDED)) {
		fb = f->body;
		const trs_body *b = &w->sys.bodies[fb];
		pf = f->mode == TRS_MODE_LANDED ? f->fixed_r : trs_body_to_fixed(b, w->t, f->r);
	}
	for (int bi = 0; bi < w->sys.count; bi++) {
		const trs_body *b = &w->sys.bodies[bi];
		for (int k = 0; k < b->nnests; k++) {
			const trs_nest *ne = &b->nests[k];
			int code = bi * TRS_MAX_NESTS + k + 1;
			/* the nest makes up its losses */
			while (w->nest_lost[bi][k] > 0 && w->t - w->nest_lost_t[bi][k] >= ne->respawn) {
				w->nest_lost[bi][k]--;
				w->nest_lost_t[bi][k] += ne->respawn;
			}
			int up = 0;
			for (int i = 0; i < w->nvessels; i++)
				up += w->vessels[i].alive && w->vessels[i].pilot == code;
			double d = fb == bi ? v3_len(v3_sub(pf, trs_nest_home(b, k))) : 1e300;
			int noise = heard_drill(w, bi, trs_nest_home(b, k), ne->hear) >= 0;
			if (d < fmax(WAKE_MIN, 3 * ne->detect) || noise) {
				int want = ne->count - w->nest_lost[bi][k];
				for (int s = up; s < want; s++)
					spawn(w, bi, k, code, s, ne->count);
			} else if (up && d > fmax(SLEEP_MIN, 5 * ne->detect) && !raiding(w, code)) {
				for (int i = 0; i < w->nvessels; i++)
					if (w->vessels[i].alive && w->vessels[i].pilot == code)
						trs_world_remove_vessel(w, i);
			}
		}
	}
}

static int raiding(const trs_world *w, int code) {
	for (int i = 0; i < w->nvessels; i++)
		if (w->vessels[i].alive && w->vessels[i].pilot == code && w->vessels[i].pilot_state == 1)
			return 1;
	return 0;
}

/* a ship of ours near, to go for: the nearest within detect of it or of
 * the nest; or, none near, a drill the nest has heard */
static trs_thing prey(const trs_world *w, const trs_vessel *me, trs_v3 home, double detect, double hear) {
	const trs_body *b = &w->sys.bodies[me->body];
	trs_v3 mine = trs_body_to_fixed(b, w->t, me->r);
	trs_thing best = { 0, 0, 0 };
	double bd = 1e300;
	for (int i = 0; i < w->nvessels; i++) {
		const trs_vessel *o = &w->vessels[i];
		if (!o->alive || !o->craft || o->is_alien || o->anchored || o->body != me->body || !trs_vessel_alive_craft(o))
			continue; /* (wreckage isn't worth their while) */
		trs_v3 p;
		if (o->mode == TRS_MODE_LANDED)
			p = o->fixed_r;
		else if (o->mode == TRS_MODE_PHYSICS)
			p = trs_body_to_fixed(b, w->t, o->r);
		else
			continue;
		double d = v3_len(v3_sub(p, mine));
		if ((d < detect || v3_len(v3_sub(p, home)) < detect) && d < bd)
			bd = d, best = (trs_thing){ TRS_THING_VESSEL, i, o->serial };
	}
	if (!best.kind) {
		int i = heard_drill(w, me->body, home, hear);
		if (i >= 0)
			best = (trs_thing){ TRS_THING_VESSEL, i, w->vessels[i].serial };
	}
	return best;
}

void trs_alien_pilot(trs_world *w, trs_vessel *vs, double h) {
	(void)h;
	int bi = (vs->pilot - 1) / TRS_MAX_NESTS, k = (vs->pilot - 1) % TRS_MAX_NESTS;
	vs->lift = v3(0, 0, 0);
	if (vs->body != bi)
		return;
	const trs_body *b = &w->sys.bodies[bi];
	const trs_nest *ne = &b->nests[k];
	trs_v3 home = trs_nest_home(b, k);
	trs_v3 rf = trs_body_to_fixed(b, w->t, vs->r);
	trs_v3 vf = trs_body_to_fixed(b, w->t, v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r)));
	/* what to go for */
	if (w->t - vs->pilot_t >= PICK_EVERY) {
		vs->pilot_t = w->t;
		trs_thing p = prey(w, vs, home, ne->detect, ne->hear);
		if (p.kind)
			vs->pilot_target = p, vs->pilot_state = 1;
		else if (vs->pilot_state == 1) {
			/* keeps after what it had until that is well away from the nest */
			trs_v3 at;
			double rad;
			const trs_thing t = vs->pilot_target;
			double keep = fmax(2 * ne->detect, t.kind == TRS_THING_VESSEL && t.id >= 0 && t.id < w->nvessels &&
					trs_vessel_noise(w, &w->vessels[t.id]) > 0 ? ne->hear : 0);
			if (!trs_thing_where(w, bi, t, &at, &rad) ||
					v3_len(v3_sub(trs_body_to_fixed(b, w->t, at), home)) > keep)
				vs->pilot_state = 0, vs->pilot_target = (trs_thing){ 0, 0, 0 };
		}
	}
	trs_v3 goal, at;
	double rad, top = PATROL_SPEED;
	int far = 0;
	if (vs->pilot_state == 1 && trs_thing_where(w, bi, vs->pilot_target, &at, &rad)) {
		trs_v3 T = trs_body_to_fixed(b, w->t, at), up, n, e;
		trs_local_frame(T, &up, &n, &e);
		double a = vs->pilot_phase + 0.12 * w->t;
		goal = v3_add(v3_madd(T, up, STANDOFF_UP), v3_scale(v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a))), STANDOFF));
		top = ATTACK_SPEED;
		far = v3_len(v3_sub(T, home)) > 2 * ne->detect;
		vs->lock = vs->pilot_target;
	} else {
		trs_v3 up, n, e;
		trs_local_frame(home, &up, &n, &e);
		double a = vs->pilot_phase + PATROL_SPEED / fmax(ne->patrol, 100) * 0.5 * w->t;
		goal = v3_add(home, v3_scale(v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a))), 0.6 * ne->patrol));
		vs->pilot_state = 0;
		vs->lock = (trs_thing){ 0, 0, 0 };
	}
	/* fly there: a speed toward it, eased in near, never into the ground */
	trs_v3 to = v3_sub(goal, rf);
	double dl = v3_len(to);
	if (far && dl > 2000)
		/* (a drill heard from far off: there fast, slowing in time to fight) */
		top = fmin(CRUISE_SPEED, fmax(ATTACK_SPEED, sqrt(2 * 8.0 * (dl - 1000))));
	trs_v3 vwant = dl > 1e-6 ? v3_scale(to, fmin(top, 0.25 * dl) / dl) : v3(0, 0, 0);
	trs_v3 up = v3_norm(rf);
	double ground = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, up) : 0;
	/* (and the ground it will be over in a few seconds: at speed, a hill
	 * comes up faster than it can climb) */
	if (b->terrain.enabled && v3_len(vf) > 30)
		ground = fmax(ground, trs_terrain_height(&b->terrain, b->radius, v3_norm(v3_madd(rf, vf, 5.0))));
	double agl = v3_len(rf) - b->radius - ground;
	double floor_agl = far && dl > 3000 ? 600.0 : FLOOR_AGL; /* (high on the way to a far fight) */
	if (agl < floor_agl)
		vwant = v3_madd(vwant, up, fmax(0, -v3_dot(vwant, up)) + (floor_agl - agl) * 0.5);
	trs_v3 af = v3_scale(v3_sub(vwant, vf), 0.8);
	/* hold itself up against gravity (and the air's drag is the gain's to take) */
	double g = b->mu / v3_len2(vs->r);
	vs->lift = v3_add(trs_body_from_fixed(b, w->t, af), v3_scale(v3_norm(vs->r), g));
	vs->sas = TRS_SAS_UPRIGHT;
	/* it fires in bursts: two seconds on, two off */
	int burst = fmod(w->t + 4 * vs->pilot_phase / (2 * TRS_PI), BURST_ON + BURST_OFF) < BURST_ON;
	vs->guard = burst;
	if (!burst)
		vs->lock = (trs_thing){ 0, 0, 0 };
}

int trs_world_drill_heard(const trs_world *w, int id, char *who, int n) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive)
		return 0;
	const trs_vessel *vs = &w->vessels[id];
	double noise = trs_vessel_noise(w, vs);
	trs_v3 at;
	if (noise <= 0 || !fixed_at(w, vs, &at))
		return 0;
	int bi = vs->body;
	const trs_body *b = &w->sys.bodies[bi];
	/* raiders on their way, hunters at it */
	for (int i = 0; i < w->nvessels; i++) {
		const trs_vessel *o = &w->vessels[i];
		if (o->alive && o->pilot && o->pilot_state == 1 && o->pilot_target.id == id && o->pilot_target.serial == vs->serial) {
			snprintf(who, n, "%s", b->nests[(o->pilot - 1) % TRS_MAX_NESTS].name);
			return TRS_HEARD_RAIDERS;
		}
	}
	if (w->wild.body == bi)
		for (int i = 0; i < w->wild.n; i++) {
			const trs_critter *c = &w->wild.c[i];
			if (c->state == TRS_CRIT_CHARGE && c->other.id == id && c->other.serial == vs->serial) {
				snprintf(who, n, "%s", b->life[c->kind].id);
				return TRS_HEARD_CHARGING;
			}
		}
	/* a nest in earshot with raiders to send */
	for (int k = 0; k < b->nnests; k++) {
		const trs_nest *ne = &b->nests[k];
		int code = bi * TRS_MAX_NESTS + k + 1, up = 0;
		for (int i = 0; i < w->nvessels; i++)
			up += w->vessels[i].alive && w->vessels[i].pilot == code;
		if (ne->hear > 0 && v3_len(v3_sub(at, trs_nest_home(b, k))) < ne->hear && (up > 0 || ne->count - w->nest_lost[bi][k] > 0)) {
			snprintf(who, n, "%s", ne->name);
			return TRS_HEARD_NEST;
		}
	}
	/* hunters within earshot */
	int k = trs_wild_hunter_near(w, bi, at, noise);
	if (k >= 0) {
		snprintf(who, n, "%s", b->life[k].id);
		return TRS_HEARD_HUNTER;
	}
	who[0] = 0;
	return TRS_HEARD_NONE;
}
