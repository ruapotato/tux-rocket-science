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

/* EVA: penguins out of their pods. A penguin on a walk is a vessel of one
 * part (an "eva" part): it falls, orbits, warps and saves like any other.
 * Walking and jumping live in the ground contact (vessel_physics.c); this
 * file puts a penguin out beside its pod and finds the hatch to get back
 * in. Who the penguin is (its name) is the game's business, not the
 * core's: the part's uid is the seat. */
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVA_GAP 0.25     /* m of clear air between the hull and the penguin */
#define EVA_GHOST_S 2.0  /* s it may overlap its pod while stepping out */
#define EVA_NEAR_GROUND 50.0 /* m: below this, a penguin stands upright to the local vertical */

void trs_local_frame(trs_v3 r, trs_v3 *up, trs_v3 *north, trs_v3 *east) {
	*up = v3_norm(r);
	trs_v3 e = v3_cross(v3(0, 0, 1), *up);
	if (v3_len2(e) < 1e-20)
		e = v3(0, 1, 0);
	*east = v3_norm(e);
	*north = v3_cross(*up, *east);
}

int trs_vessel_is_eva(const trs_vessel *vs) {
	return vs->craft && vs->craft->count == 1 && trs_craft_def(vs->craft, 0)->is_eva;
}

/* attitude and spin of a vessel in its SOI frame, in any mode */
static void attitude_of(const trs_world *w, const trs_vessel *vs, trs_quat *q, trs_v3 *om) {
	if (vs->mode == TRS_MODE_LANDED) {
		const trs_body *b = &w->sys.bodies[vs->body];
		*q = quat_normalize(quat_mul(trs_body_quat(b, w->t), vs->fixed_q));
		*om = trs_body_omega(b);
	} else {
		*q = vs->q;
		*om = vs->w;
	}
}

int trs_world_eva_exit(trs_world *w, int id, int part, int penguin_def) {
	if (id < 0 || id >= w->nvessels || !w->vessels[id].alive || !w->vessels[id].craft)
		return -1;
	trs_vessel *vs = &w->vessels[id];
	const trs_craft *c = vs->craft;
	if (part < 0 || part >= c->count || trs_craft_def(c, part)->crew <= 0 || penguin_def < 0 ||
			penguin_def >= c->cat->count || !c->cat->defs[penguin_def].is_eva)
		return -1;
	if (vs->dirty)
		trs_vessel_refresh(w, id);
	const trs_part *p = &c->parts[part];
	const trs_part_def *d = trs_craft_def(c, part);
	const trs_part_def *pd = &c->cat->defs[penguin_def];
	/* out through the side of the part, away from the craft's axis (or
	 * along its local +Z when the part sits on the axis) */
	trs_v3 out = v3_sub(p->pos, vs->cm);
	out.y = 0;
	if (v3_len(out) < 0.05)
		out = quat_rotate(p->rot, v3(0, 0, 1));
	out.y = 0;
	if (v3_len(out) < 1e-6)
		out = v3(0, 0, 1);
	out = v3_norm(out);
	trs_v3 local = v3_add(p->pos, v3_scale(out, fmax(d->radius_bottom, d->radius_top) + EVA_GAP + pd->radius));
	trs_v3 r0, v0, om;
	trs_quat q;
	trs_vessel_state(w, id, &r0, &v0);
	attitude_of(w, vs, &q, &om);
	trs_v3 arm = quat_rotate(q, v3_sub(local, vs->cm));
	trs_v3 r = v3_add(r0, arm), v = v3_add(v0, v3_cross(om, arm));
	const trs_body *b = &w->sys.bodies[vs->body];
	double above = v3_len(r) - b->radius - trs_body_terrain_at(b, w->t, r);
	/* a craft standing on the ground with a ladder: the penguin climbs down
	 * it to the ground (out of a tall lander's hatch it would fall) */
	int ladder = 0;
	for (int i = 0; i < c->count; i++)
		ladder |= trs_craft_def(c, i)->is_ladder;
	if (ladder && (vs->landed || vs->mode == TRS_MODE_LANDED) && above > pd->height * 0.5 + 0.3) {
		r = v3_scale(r, (v3_len(r) - above + pd->height * 0.5 + 0.02) / v3_len(r));
		v = v3_cross(trs_body_omega(b), r);
		above = pd->height * 0.5 + 0.02;
	}
	trs_v3 up = above < EVA_NEAR_GROUND ? v3_norm(r) : quat_rotate(q, v3(0, 1, 0));
	if (above < EVA_NEAR_GROUND) {
		/* feet no lower than the ground under it */
		double feet = above - pd->height * 0.5;
		if (feet < 0.02)
			r = v3_add(r, v3_scale(up, 0.02 - feet));
	}
	trs_craft pen;
	trs_craft_init(&pen, c->cat);
	trs_craft_add(&pen, penguin_def, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	snprintf(pen.name, sizeof pen.name, "%s", "EVA");
	int nid = trs_world_add_craft(w, &pen, vs->body, r, v, quat_from_to(v3(0, 1, 0), up));
	trs_craft_free(&pen);
	if (nid < 0)
		return -1;
	trs_vessel *nv = &w->vessels[nid];
	vs = &w->vessels[id];
	nv->lifted = vs->lifted, nv->liftoff_t = vs->liftoff_t;
	nv->w = above < EVA_NEAR_GROUND ? trs_body_omega(b) : om;
	nv->sas = above < EVA_NEAR_GROUND ? TRS_SAS_UPRIGHT : TRS_SAS_STABILITY;
	nv->sas_hold = nv->q;
	nv->fresh = 1;
	trs_vessel_add_ghost(nv, id, w->t + EVA_GHOST_S);
	trs_vessel_add_ghost(vs, nid, w->t + EVA_GHOST_S);
	return nid;
}

#define FLAG_AHEAD 0.8 /* m from the penguin's centre to the pole */

int trs_world_plant_flag(trs_world *w, int eva, int flag_def) {
	if (eva < 0 || eva >= w->nvessels || !w->vessels[eva].alive || !trs_vessel_is_eva(&w->vessels[eva]))
		return -1;
	trs_vessel *ev = &w->vessels[eva];
	const trs_part_catalog *cat = ev->craft->cat;
	if (flag_def < 0 || flag_def >= cat->count || ev->contacts == 0 || ev->mode != TRS_MODE_PHYSICS)
		return -1;
	const trs_body *b = &w->sys.bodies[ev->body];
	trs_v3 up, north, east;
	trs_local_frame(ev->r, &up, &north, &east);
	/* ahead of the penguin: the way its own +Z faces, flat on the ground */
	trs_v3 fwd = quat_rotate(ev->q, v3(0, 0, 1));
	fwd = v3_sub(fwd, v3_scale(up, v3_dot(fwd, up)));
	fwd = v3_len(fwd) > 1e-6 ? v3_norm(fwd) : north;
	trs_v3 at = v3_add(ev->r, v3_scale(fwd, FLAG_AHEAD));
	const trs_part_def *fd = &cat->defs[flag_def];
	double ground = trs_body_terrain_at(b, w->t, at);
	at = v3_scale(v3_norm(at), b->radius + ground + fd->height * 0.5);
	trs_craft c;
	trs_craft_init(&c, cat);
	trs_craft_add(&c, flag_def, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	snprintf(c.name, sizeof c.name, "%s", "Flag");
	int id = trs_world_add_craft(w, &c, ev->body, at, v3_cross(trs_body_omega(b), at), quat_from_to(v3(0, 1, 0), v3_norm(at)));
	trs_craft_free(&c);
	if (id < 0)
		return -1;
	trs_vessel *fv = &w->vessels[id];
	fv->w = trs_body_omega(b);
	trs_vessel_set_mode(w, id, TRS_MODE_LANDED);
	fv->anchored = 1;
	fv->landed = 1;
	fv->sas = TRS_SAS_OFF;
	return id;
}

int trs_eva_hatch(const trs_world *w, int eva, int target, double *dist) {
	*dist = INFINITY;
	if (eva < 0 || target < 0 || eva >= w->nvessels || target >= w->nvessels || eva == target)
		return -1;
	const trs_vessel *ev = &w->vessels[eva], *tv = &w->vessels[target];
	if (!ev->alive || !tv->alive || !trs_vessel_is_eva(ev) || !tv->craft || ev->body != tv->body)
		return -1;
	trs_v3 re, ve, rt, vt, om;
	trs_quat q;
	trs_vessel_state(w, eva, &re, &ve);
	trs_vessel_state(w, target, &rt, &vt);
	attitude_of(w, tv, &q, &om);
	const trs_part_def *ed = trs_craft_def(ev->craft, 0);
	/* in at the crewed part nearest to it, if the penguin can reach that
	 * part, or a ladder on the craft (it climbs the ladder to the hatch) */
	trs_v3 local = v3_add(quat_rotate(quat_conj(q), v3_sub(re, rt)), tv->cm);
	int best = -1;
	double best_crew = INFINITY;
	for (int i = 0; i < tv->craft->count; i++) {
		const trs_part_def *d = trs_craft_def(tv->craft, i);
		if (d->is_eva || !(d->crew > 0 || d->is_ladder))
			continue;
		const trs_part *p = &tv->craft->parts[i];
		trs_v3 lp = quat_rotate(quat_conj(p->rot), v3_sub(local, p->pos));
		double ay = fmax(0, fabs(lp.y) - d->height * 0.5);
		double ar = fmax(0, sqrt(lp.x * lp.x + lp.z * lp.z) - fmax(d->radius_bottom, d->radius_top));
		double gap = sqrt(ay * ay + ar * ar) - ed->radius;
		*dist = fmin(*dist, gap);
		if (d->crew > 0 && v3_len(v3_sub(local, p->pos)) < best_crew) {
			best_crew = v3_len(v3_sub(local, p->pos));
			best = i;
		}
	}
	return *dist <= ed->eva_reach ? best : -1;
}
