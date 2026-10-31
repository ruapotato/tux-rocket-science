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

/* The wild: the animals near the player, alive to what goes on round them.
 * Life on the ground (life.c) says where every animal is and the loop it
 * walks; an animal walks it on a clock of its own (trs_life_animal_at), so
 * the scenery far off and the live animal near agree on where it is. Those
 * within LIVE_R of the focus (the vessel the player is with) are taken up
 * here: undisturbed, they keep to their loops; a shy one runs from anyone
 * near, a curious one trots up to a penguin and stays at its heels, a
 * hostile one goes for the nearest penguin and bites, and any of them hurt
 * runs from whoever did it (or, hostile and hurt by a penguin, goes for
 * them). A ship they leave be: aboard it, a penguin is safe; unless it
 * is digging: a hunter within earshot of a drill (its noise_m) comes for
 * the drill, and bites it till the noise stops or it is driven off. Hurt
 * enough, one goes down; its herd makes up the loss in DEAD_S. They move at
 * WILD_HZ on the terrain's own height. */
#include "trs/world.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define WILD_DT 0.1         /* s: they move ten times a second */
#define LIVE_R 450.0        /* m from the focus: animals this near are alive to it */
#define DROP_R 650.0        /* ... and are let go past this */
#define SCAN_EVERY 1.0      /* s between looking for who has come near */
#define FOCUS_MAX_AGL 2500.0 /* the focus higher than this over the ground: the wild sleeps */
#define DOWN_S 6.0          /* s a zapped animal lies there */
#define DEAD_S (6 * 3600.0) /* s until its herd has made up the loss */
#define TURN_ACCEL 6.0      /* m/s2 they speed up, slow down and turn with */
#define FOLLOW_SEE 30.0     /* m: a curious one notices a penguin this near */
#define FOLLOW_LOSE 45.0
#define BITE_EVERY 1.2      /* s between bites */
#define LOOP_OMEGA 0.21     /* rad/s of a walker's stop-and-go */

/* ---- the loop clock ---------------------------------------------------- */

static trs_v3 catmull(trs_v3 p0, trs_v3 p1, trs_v3 p2, trs_v3 p3, double t) {
	double t2 = t * t, t3 = t2 * t;
	trs_v3 r;
	r.x = 0.5 * (2 * p1.x + (-p0.x + p2.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
	r.y = 0.5 * (2 * p1.y + (-p0.y + p2.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
	r.z = 0.5 * (2 * p1.z + (-p0.z + p2.z) * t + (2 * p0.z - 5 * p1.z + 4 * p2.z - p3.z) * t2 + (-p0.z + 3 * p1.z - 3 * p2.z + p3.z) * t3);
	return r;
}

/* A walker's pace goes ((1 + sin x) / 2)^2 of its walk (x = LOOP_OMEGA t +
 * 3 x its phase): a stroll, a slow-down, a moment's stop to graze. The
 * distance it has gone is that's integral, so the clock needs no history:
 * 3t/8 - cos(x) / (2w) - sin(2x) / (16w). A flyer never stops. */
void trs_life_animal_at(const trs_life *L, const trs_life_instance *li, trs_v3 centre, double t, trs_v3 *pos, trs_v3 *fwd) {
	int n = li->nloop;
	trs_v3 up = v3_norm(v3_add(centre, li->pos));
	if (n < 2) {
		*pos = v3_add(centre, li->pos);
		trs_v3 e = v3_cross(v3(0, 0, 1), up);
		*fwd = v3_len2(e) > 1e-20 ? v3_norm(e) : v3(0, 1, 0);
		return;
	}
	double seg = fmax(v3_len(v3_sub(li->loop[1], li->loop[0])), 0.1);
	double phase = li->yaw * 2 * TRS_PI, gone;
	if (L->fly > 0)
		gone = t;
	else {
		double x = LOOP_OMEGA * t + 3 * phase;
		gone = 3 * t / 8 - cos(x) / (2 * LOOP_OMEGA) - sin(2 * x) / (16 * LOOP_OMEGA);
	}
	double s = fmod(li->scale * n + L->speed * gone / seg, (double)n);
	if (s < 0)
		s += n;
	int i = (int)s;
	if (i >= n)
		i = n - 1;
	double f = s - i;
	trs_v3 p0 = li->loop[(i - 1 + n) % n], p1 = li->loop[i], p2 = li->loop[(i + 1) % n], p3 = li->loop[(i + 2) % n];
	trs_v3 at = catmull(p0, p1, p2, p3, f);
	trs_v3 ahead = f + 0.05 > 1 ? v3_sub(p2, p1) : v3_sub(catmull(p0, p1, p2, p3, f + 0.05), at);
	*pos = v3_add(centre, at);
	up = v3_norm(*pos);
	ahead = v3_madd(ahead, up, -v3_dot(ahead, up));
	*fwd = v3_len2(ahead) > 1e-12 ? v3_norm(ahead) : v3_norm(v3_cross(v3(0, 0, 1), up));
}

/* ---- who is near ------------------------------------------------------- */

const trs_life *trs_critter_kind(const trs_world *w, const trs_critter *c) {
	return &w->sys.bodies[w->wild.body].life[c->kind];
}

int trs_wild_find(const trs_world *w, int id) {
	for (int i = 0; i < w->wild.n; i++)
		if (w->wild.c[i].id == id)
			return i;
	return -1;
}

static void drop_cells(trs_wild *wd) {
	for (int k = 0; k < wd->ncells; k++)
		free(wd->cells[k].inst);
	wd->ncells = 0;
}

void trs_wild_clear(trs_world *w) {
	trs_wild *wd = &w->wild;
	/* (the downed are still down when the wild wakes again) */
	for (int i = 0; i < wd->n; i++)
		if (wd->c[i].state == TRS_CRIT_DOWN && wd->ndead < TRS_WILD_DEAD)
			wd->dead[wd->ndead++] = (trs_wild_dead){ wd->body, wd->c[i].cell, wd->c[i].kind, wd->c[i].idx, w->t + DEAD_S };
	drop_cells(wd);
	wd->n = 0;
	wd->body = -1;
}

void trs_world_set_focus(trs_world *w, int id) {
	if (id >= 0 && id < w->nvessels && w->vessels[id].alive)
		w->focus = id, w->focus_serial = w->vessels[id].serial;
	else
		w->focus = -1, w->focus_serial = 0;
}

/* body-fixed position of a vessel near the ground (0 if on rails or between the stars) */
static int vessel_fixed(const trs_world *w, const trs_vessel *vs, trs_v3 *p) {
	if (vs->mode == TRS_MODE_LANDED) {
		*p = vs->fixed_r;
		return 1;
	}
	if (vs->mode != TRS_MODE_PHYSICS)
		return 0;
	*p = trs_body_to_fixed(&w->sys.bodies[vs->body], w->t, vs->r);
	return 1;
}

static int same_patch(trs_patch_id a, trs_patch_id b) {
	return a.face == b.face && a.level == b.level && a.i == b.i && a.j == b.j;
}

/* the instances of kind k in cell p, generated once while the cell is near */
static int cell_of(trs_world *w, trs_patch_id p, int k, trs_v3 *centre, trs_life_instance **inst) {
	trs_wild *wd = &w->wild;
	for (int q = 0; q < wd->ncells; q++)
		if (wd->cells[q].kind == k && same_patch(wd->cells[q].p, p)) {
			*centre = wd->cells[q].centre;
			*inst = wd->cells[q].inst;
			return wd->cells[q].n;
		}
	if (wd->ncells >= (int)(sizeof wd->cells / sizeof wd->cells[0]))
		return 0;
	const trs_body *b = &w->sys.bodies[wd->body];
	int max = 400;
	trs_life_instance *buf = malloc(sizeof(trs_life_instance) * (size_t)max);
	trs_v3 c;
	int n = trs_life_cell(b, p, 1u << k, &c, buf, max);
	int q = wd->ncells++;
	wd->cells[q].p = p, wd->cells[q].kind = k, wd->cells[q].n = n, wd->cells[q].centre = c, wd->cells[q].inst = buf;
	*centre = c;
	*inst = buf;
	return n;
}

static int is_dead(trs_world *w, trs_patch_id p, int k, int idx) {
	trs_wild *wd = &w->wild;
	for (int q = 0; q < wd->ndead; q++)
		if (wd->dead[q].body == wd->body && wd->dead[q].kind == k && wd->dead[q].idx == idx && same_patch(wd->dead[q].cell, p))
			return 1;
	return 0;
}

/* the loudest drill near the focus: where (body-fixed) and how far it is
 * heard (0: all quiet) */
static double noise_near(const trs_world *w, trs_v3 pf, trs_v3 *at) {
	double loud = 0;
	for (int i = 0; i < w->nvessels; i++) {
		const trs_vessel *vs = &w->vessels[i];
		trs_v3 p;
		double n = vs->alive && vs->body == w->wild.body ? trs_vessel_noise(w, vs) : 0;
		if (n > loud && vessel_fixed(w, vs, &p) && v3_len(v3_sub(p, pf)) < DROP_R + n)
			loud = n, *at = p;
	}
	return loud;
}

static void scan(trs_world *w, trs_v3 pf) {
	trs_wild *wd = &w->wild;
	const trs_body *b = &w->sys.bodies[wd->body];
	/* (a drill digging: the hunters within its earshot are alive to it too) */
	trs_v3 np = pf;
	double noise = noise_near(w, pf, &np);
	/* the herds have made up old losses */
	for (int q = 0; q < wd->ndead; q++)
		if (wd->dead[q].until <= w->t)
			wd->dead[q--] = wd->dead[--wd->ndead];
	/* let go of the far ones */
	for (int i = 0; i < wd->n; i++)
		if (wd->c[i].state != TRS_CRIT_DOWN && v3_len(v3_sub(wd->c[i].pos, pf)) > DROP_R &&
				!(noise > 0 && b->life[wd->c[i].kind].temper == TRS_TEMPER_HOSTILE && v3_len(v3_sub(wd->c[i].pos, np)) < noise + 200))
			wd->c[i--] = wd->c[--wd->n];
	trs_patch_id near[64];
	double reach = fmax(LIVE_R, noise > 0 ? v3_len(v3_sub(np, pf)) + noise : 0);
	int nn = trs_life_cells_near(b, v3_norm(pf), reach, near, 64);
	/* forget cells no longer near */
	for (int q = 0; q < wd->ncells; q++) {
		int keep = 0;
		for (int k = 0; k < nn && !keep; k++)
			keep = same_patch(near[k], wd->cells[q].p);
		if (!keep) {
			free(wd->cells[q].inst);
			wd->cells[q--] = wd->cells[--wd->ncells];
		}
	}
	for (int k = 0; k < b->nlife; k++) {
		const trs_life *L = &b->life[k];
		if (!L->fauna)
			continue;
		for (int c = 0; c < nn; c++) {
			trs_v3 centre;
			trs_life_instance *inst;
			int n = cell_of(w, near[c], k, &centre, &inst);
			for (int idx = 0; idx < n; idx++) {
				trs_v3 pos, fwd;
				trs_life_animal_at(L, &inst[idx], centre, w->t, &pos, &fwd);
				int hears = noise > 0 && L->temper == TRS_TEMPER_HOSTILE && v3_len(v3_sub(pos, np)) < noise;
				if ((v3_len(v3_sub(pos, pf)) > LIVE_R && !hears) || wd->n >= TRS_WILD_MAX)
					continue;
				int live = 0;
				for (int i = 0; i < wd->n && !live; i++)
					live = wd->c[i].kind == k && wd->c[i].idx == idx && same_patch(wd->c[i].cell, near[c]);
				if (live || is_dead(w, near[c], k, idx))
					continue;
				trs_critter *cr = &wd->c[wd->n++];
				memset(cr, 0, sizeof *cr);
				cr->id = ++wd->next_id;
				cr->cell = near[c], cr->kind = k, cr->idx = idx;
				cr->state = TRS_CRIT_ROAM;
				cr->t_state = w->t;
				cr->pos = pos, cr->fwd = fwd;
				cr->size = L->size_min + (L->size_max - L->size_min) * inst[idx].scale;
				cr->hp = L->hp;
				cr->centre = centre;
				cr->inst = inst[idx];
			}
		}
	}
}

/* ---- what they do ------------------------------------------------------ */

typedef struct {
	int id, serial;
	trs_v3 p;
	double radius;
	int eva, alien;
	double noise;  /* m its drill is heard (0: quiet) */
	trs_v3 drill;  /* body-fixed: the noisy drill, what a hunter goes for */
	int drill_part;
} actor;

static trs_thing vessel_thing(const trs_world *w, int id) {
	return (trs_thing){ TRS_THING_VESSEL, id, w->vessels[id].serial };
}

static void set_state(const trs_world *w, trs_critter *c, int st, trs_thing other) {
	if (c->state == TRS_CRIT_ROAM && c->state != st) {
		/* off its loop: it carries on from where it is, as fast as it was going */
		const trs_life *L = trs_critter_kind(w, c);
		c->vel = v3_scale(c->fwd, L->speed);
	}
	if (c->state != st)
		c->t_state = w->t;
	c->state = st;
	c->other = other;
}

void trs_critter_hurt(trs_world *w, int i, double hp, trs_thing by) {
	trs_critter *c = &w->wild.c[i];
	if (c->state == TRS_CRIT_DOWN)
		return;
	c->hp -= hp;
	if (c->hp <= 0) {
		c->hp = 0;
		set_state(w, c, TRS_CRIT_DOWN, by);
		c->vel = v3(0, 0, 0);
		return;
	}
	const trs_life *L = trs_critter_kind(w, c);
	int penguin = by.kind == TRS_THING_VESSEL && by.id >= 0 && by.id < w->nvessels && w->vessels[by.id].alive &&
			trs_vessel_is_eva(&w->vessels[by.id]);
	if (L->temper == TRS_TEMPER_HOSTILE && c->hp > 0.25 * L->hp && penguin)
		set_state(w, c, TRS_CRIT_CHARGE, by);
	else if (c->state != TRS_CRIT_FLEE || c->other.kind == TRS_THING_NONE)
		set_state(w, c, TRS_CRIT_FLEE, by);
}

static int thing_pos(const trs_world *w, const actor *a, int na, trs_thing th, trs_v3 *p, double *radius) {
	if (th.kind != TRS_THING_VESSEL)
		return 0;
	for (int k = 0; k < na; k++)
		if (a[k].id == th.id && a[k].serial == th.serial) {
			*p = a[k].p;
			*radius = a[k].radius;
			if (a[k].drill_part >= 0) /* (a ship digging: the drill is what it goes for) */
				*p = a[k].drill, *radius = 0.3;
			return 1;
		}
	(void)w;
	return 0;
}

static const actor *actor_of(const actor *a, int na, trs_thing th) {
	for (int k = 0; k < na; k++)
		if (th.kind == TRS_THING_VESSEL && a[k].id == th.id && a[k].serial == th.serial)
			return &a[k];
	return NULL;
}

/* the running drill nearest the ground, body-fixed under it (-1: none) */
static int low_drill(const trs_world *w, const trs_vessel *vs, trs_v3 *at) {
	const trs_body *b = &w->sys.bodies[vs->body];
	int best = -1;
	double bg = 1e300;
	for (int i = 0; i < vs->craft->count; i++) {
		const trs_part_def *d = trs_craft_def(vs->craft, i);
		if (d->drill_kg_s <= 0 || !vs->craft->parts[i].running)
			continue;
		trs_v3 arm = v3_sub(vs->craft->parts[i].pos, vs->cm), p;
		if (vs->mode == TRS_MODE_LANDED)
			p = v3_add(vs->fixed_r, quat_rotate(vs->fixed_q, arm));
		else
			p = trs_body_to_fixed(b, w->t, v3_add(vs->r, quat_rotate(vs->q, arm)));
		double ground = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, v3_norm(p)) : 0;
		double gap = v3_len(p) - b->radius - ground;
		if (gap < bg)
			bg = gap, best = i, *at = v3_scale(v3_norm(p), b->radius + ground);
	}
	return best;
}

static double hashf(int id, int k) {
	unsigned x = (unsigned)id * 2654435761u ^ (unsigned)k * 40503u;
	x ^= x >> 15, x *= 2246822519u, x ^= x >> 13;
	return (x & 0xffffff) / 16777216.0;
}

static void move(const trs_world *w, trs_critter *c, trs_v3 want, double dt) {
	const trs_body *b = &w->sys.bodies[w->wild.body];
	const trs_life *L = trs_critter_kind(w, c);
	trs_v3 up = v3_norm(c->pos);
	want = v3_madd(want, up, -v3_dot(want, up));
	trs_v3 dv = v3_sub(want, c->vel);
	double dl = v3_len(dv), most = TURN_ACCEL * dt;
	if (dl > most)
		dv = v3_scale(dv, most / dl);
	c->vel = v3_add(c->vel, dv);
	trs_v3 p = v3_madd(c->pos, c->vel, dt);
	trs_v3 d = v3_norm(p);
	double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, d) : 0;
	c->pos = v3_scale(d, b->radius + h + L->fly);
	c->vel = v3_madd(c->vel, d, -v3_dot(c->vel, d));
	if (v3_len(c->vel) > 0.05)
		c->fwd = v3_norm(c->vel);
	else
		c->fwd = v3_norm(v3_madd(c->fwd, d, -v3_dot(c->fwd, d)));
}

static void step(trs_world *w, double dt) {
	trs_wild *wd = &w->wild;
	const trs_body *b = &w->sys.bodies[wd->body];
	/* who is about: every vessel near on this body */
	actor act[64];
	int na = 0;
	trs_v3 pf = v3(0, 0, 0);
	vessel_fixed(w, &w->vessels[w->focus], &pf);
	for (int i = 0; i < w->nvessels && na < 64; i++) {
		const trs_vessel *vs = &w->vessels[i];
		trs_v3 p;
		if (!vs->alive || !vs->craft || vs->body != wd->body || !vessel_fixed(w, vs, &p))
			continue;
		double noise = vs->is_alien ? 0 : trs_vessel_noise(w, vs);
		if (v3_len(v3_sub(p, pf)) > DROP_R + 300 + noise)
			continue;
		actor *a = &act[na++];
		a->id = i, a->serial = vs->serial, a->p = p;
		a->eva = trs_vessel_is_eva(vs);
		a->alien = vs->is_alien;
		a->noise = noise;
		a->drill_part = noise > 0 ? low_drill(w, vs, &a->drill) : -1;
		a->radius = a->eva ? 0.35 : fmax(0.5, vs->bound_radius * 0.6);
		/* (the one it sits round is the centre of mass; the parts reach
		 * further, but most of a lander is within this of it) */
	}
	for (int i = 0; i < wd->n; i++) {
		trs_critter *c = &wd->c[i];
		const trs_life *L = trs_critter_kind(w, c);
		if (c->state == TRS_CRIT_DOWN) {
			if (w->t - c->t_state >= DOWN_S) {
				if (wd->ndead < TRS_WILD_DEAD)
					wd->dead[wd->ndead++] = (trs_wild_dead){ wd->body, c->cell, c->kind, c->idx, w->t + DEAD_S };
				wd->c[i--] = wd->c[--wd->n];
			}
			continue;
		}
		/* the nearest penguin, and the nearest ship not the Jaegers' */
		int pen = -1, ship = -1;
		double dp = 1e300, ds = 1e300;
		for (int k = 0; k < na; k++) {
			double d = v3_len(v3_sub(act[k].p, c->pos)) - act[k].radius;
			if (act[k].eva && d < dp)
				dp = d, pen = k;
			else if (!act[k].eva && !act[k].alien && d < ds)
				ds = d, ship = k;
		}
		/* what it makes of them */
		if (c->state == TRS_CRIT_ROAM || c->state == TRS_CRIT_WANDER) {
			if (L->temper == TRS_TEMPER_SHY) {
				int near = dp < ds ? pen : ship;
				if (near >= 0 && fmin(dp, ds) < L->wary)
					set_state(w, c, TRS_CRIT_FLEE, vessel_thing(w, act[near].id));
			} else if (L->temper == TRS_TEMPER_CURIOUS) {
				if (pen >= 0 && dp < FOLLOW_SEE)
					set_state(w, c, TRS_CRIT_FOLLOW, vessel_thing(w, act[pen].id));
			} else if (L->temper == TRS_TEMPER_HOSTILE) {
				if (pen >= 0 && dp < L->aggro)
					set_state(w, c, TRS_CRIT_CHARGE, vessel_thing(w, act[pen].id));
				else
					/* a drill digging within earshot: it comes for the noise */
					for (int k = 0; k < na; k++)
						if (act[k].drill_part >= 0 && v3_len(v3_sub(act[k].drill, c->pos)) < act[k].noise) {
							set_state(w, c, TRS_CRIT_CHARGE, vessel_thing(w, act[k].id));
							break;
						}
			}
		}
		if (c->state == TRS_CRIT_ROAM) {
			trs_life_animal_at(L, &c->inst, c->centre, w->t, &c->pos, &c->fwd);
			c->vel = v3(0, 0, 0);
			continue;
		}
		trs_v3 op;
		double orad;
		int there = thing_pos(w, act, na, c->other, &op, &orad);
		trs_v3 to = there ? v3_sub(op, c->pos) : v3(0, 0, 0);
		double dist = there ? v3_len(to) - orad : 1e300;
		trs_v3 dir = there && v3_len(to) > 1e-6 ? v3_scale(to, 1.0 / v3_len(to)) : c->fwd;
		trs_v3 want = v3(0, 0, 0);
		switch (c->state) {
		case TRS_CRIT_FLEE: {
			double calm = L->temper == TRS_TEMPER_SHY ? 2 * L->wary : 60.0;
			if ((!there || dist > calm) && w->t - c->t_state > 4) {
				set_state(w, c, TRS_CRIT_WANDER, (trs_thing){ 0, 0, 0 });
				break;
			}
			want = v3_scale(dir, -L->run);
			break;
		}
		case TRS_CRIT_FOLLOW: {
			if (!there || dist > FOLLOW_LOSE) {
				set_state(w, c, TRS_CRIT_WANDER, (trs_thing){ 0, 0, 0 });
				break;
			}
			double stop = 1.2 + 0.5 * c->size;
			want = dist > stop ? v3_scale(dir, fmin(L->speed * 3, 0.8 * (dist - stop) + 0.3)) : v3(0, 0, 0);
			if (dist <= stop)
				c->fwd = dir;
			break;
		}
		case TRS_CRIT_CHARGE: {
			const actor *tg = actor_of(act, na, c->other);
			int at_drill = tg && !tg->eva && tg->drill_part >= 0;
			if (tg && !tg->eva && !at_drill) {
				/* the noise has stopped: nothing there to go for any more */
				set_state(w, c, TRS_CRIT_WANDER, (trs_thing){ 0, 0, 0 });
				break;
			}
			if (at_drill) /* (along the ground to it: the drill is a little above it) */
				dist = fmax(0, v3_len(v3_madd(to, v3_norm(c->pos), -v3_dot(to, v3_norm(c->pos)))) - orad);
			if (!there || dist > (at_drill ? tg->noise * 1.2 : 2 * L->aggro)) {
				set_state(w, c, TRS_CRIT_WANDER, (trs_thing){ 0, 0, 0 });
				break;
			}
			if (c->hp < 0.25 * L->hp) {
				set_state(w, c, TRS_CRIT_FLEE, c->other);
				break;
			}
			double reach = 0.4 + 0.5 * c->size;
			if (dist > reach)
				want = v3_scale(dir, fmin(L->run, 2.0 * (dist - reach) + 0.5));
			else {
				c->fwd = dir;
				if (w->t >= c->next_bite) {
					c->next_bite = w->t + BITE_EVERY;
					int vid = c->other.id;
					trs_part_hurt(w, vid, at_drill ? tg->drill_part : 0, L->bite, TRS_DESTROY_BITTEN);
				}
			}
			break;
		}
		case TRS_CRIT_WANDER: {
			/* an amble that turns now and then, all its own */
			double a = hashf(c->id, 1) * 2 * TRS_PI + 0.8 * sin(w->t * 0.07 + c->id);
			double pace = 0.5 * L->speed * fmax(0, sin(w->t * 0.19 + 5 * hashf(c->id, 2)));
			trs_v3 up = v3_norm(c->pos), e = v3_cross(v3(0, 0, 1), up);
			e = v3_len2(e) > 1e-20 ? v3_norm(e) : v3(0, 1, 0);
			trs_v3 n = v3_cross(up, e);
			want = v3_scale(v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a))), pace);
			break;
		}
		default:
			break;
		}
		move(w, c, want, dt);
	}
	(void)b;
}

void trs_wild_step(trs_world *w, double dt) {
	trs_wild *wd = &w->wild;
	const trs_vessel *f = w->focus >= 0 && w->focus < w->nvessels ? &w->vessels[w->focus] : NULL;
	trs_v3 pf = v3(0, 0, 0);
	int awake = f && f->alive && f->serial == w->focus_serial && vessel_fixed(w, f, &pf);
	if (awake) {
		const trs_body *b = &w->sys.bodies[f->body];
		double h = b->terrain.enabled ? trs_terrain_height(&b->terrain, b->radius, v3_norm(pf)) : 0;
		awake = b->nlife > 0 && v3_len(pf) - b->radius - h < FOCUS_MAX_AGL;
	}
	if (!awake) {
		if (wd->n || wd->ncells)
			trs_wild_clear(w);
		return;
	}
	if (wd->body != f->body) {
		trs_wild_clear(w);
		wd->body = f->body;
		wd->scan_t = -1e300;
	}
	wd->acc += dt;
	if (w->t - wd->scan_t >= SCAN_EVERY) {
		wd->scan_t = w->t;
		scan(w, pf);
	}
	while (wd->acc >= WILD_DT - 1e-9) {
		wd->acc -= WILD_DT;
		step(w, WILD_DT);
	}
}

int trs_wild_hunter_near(const trs_world *w, int bi, trs_v3 at, double r) {
	const trs_body *b = &w->sys.bodies[bi];
	const trs_wild *wd = &w->wild;
	trs_patch_id near[64];
	int nn = 0, found = -1;
	trs_life_instance *buf = NULL;
	for (int k = 0; k < b->nlife && found < 0; k++) {
		const trs_life *L = &b->life[k];
		if (!L->fauna || L->temper != TRS_TEMPER_HOSTILE)
			continue;
		if (!buf) {
			nn = trs_life_cells_near(b, v3_norm(at), r, near, 64);
			buf = malloc(sizeof(trs_life_instance) * 400);
		}
		for (int c = 0; c < nn && found < 0; c++) {
			trs_v3 centre;
			int n = trs_life_cell(b, near[c], 1u << k, &centre, buf, 400);
			for (int idx = 0; idx < n && found < 0; idx++) {
				trs_v3 pos, fwd;
				trs_life_animal_at(L, &buf[idx], centre, w->t, &pos, &fwd);
				if (v3_len(v3_sub(pos, at)) > r)
					continue;
				int dead = 0;
				for (int q = 0; q < wd->ndead && !dead; q++)
					dead = wd->dead[q].body == bi && wd->dead[q].kind == k && wd->dead[q].idx == idx && same_patch(wd->dead[q].cell, near[c]) &&
							wd->dead[q].until > w->t;
				for (int q = 0; q < wd->n && !dead && wd->body == bi; q++)
					dead = wd->c[q].state == TRS_CRIT_DOWN && wd->c[q].kind == k && wd->c[q].idx == idx && same_patch(wd->c[q].cell, near[c]);
				if (!dead)
					found = k;
			}
		}
	}
	free(buf);
	return found;
}
