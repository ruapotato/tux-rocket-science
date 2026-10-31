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

/* TrsSim methods for the wild, lasers and the Jaegers. Positions come and
 * go relative to an origin vessel's centre of mass in its SOI frame, like
 * vessel_render; the wild's are body-fixed (TerrainLife draws them in the
 * cells' own frames). */
#include "sim.h"
#include "trs/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static trs_sim_handle *H(void *self) { return self; }

static int ok_vessel(trs_sim_handle *h, int64_t id) {
	return h->world && id >= 0 && id < h->world->nvessels && h->world->vessels[id].alive;
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

/* the origin vessel's CoM, SOI frame */
static trs_v3 origin_r(const trs_world *w, int origin) {
	trs_v3 r, v;
	trs_vessel_state(w, origin, &r, &v);
	return r;
}

static const char *STATES[] = { "roaming", "wandering", "fleeing", "following", "charging", "down" };
static const char *TEMPERS[] = { "calm", "shy", "curious", "hostile" };

/* set_focus(id): the vessel the player is with (the wild wakes round it) */
static void m_set_focus(void *self, const trs_val *a, trs_val *r) {
	(void)r;
	trs_sim_handle *h = H(self);
	if (h->world)
		trs_world_set_focus(h->world, (int)a[0].i);
}

#define WPP 20
/* wild_list() -> [body, n, then per live animal: id, face, level, i, j,
 * kind, idx, state, pos(3), vel(3), fwd(3) (body-fixed), hp share, s in
 * its state, size: WPP doubles each; then m, and the m downed ones the
 * herds haven't made up yet on that body: face, level, i, j, kind, idx] */
static void m_wild_list(void *self, const trs_val *a, trs_val *r) {
	(void)a;
	trs_sim_handle *h = H(self);
	if (!h->world || h->world->wild.body < 0) {
		double d[2] = { -1, 0 };
		ret_doubles(r, d, 2);
		return;
	}
	const trs_wild *wd = &h->world->wild;
	double *d = malloc(sizeof(double) * (size_t)(3 + wd->n * WPP + wd->ndead * 6));
	d[0] = wd->body, d[1] = wd->n;
	for (int i = 0; i < wd->n; i++) {
		const trs_critter *c = &wd->c[i];
		const trs_life *L = trs_critter_kind(h->world, c);
		double *o = d + 2 + i * WPP;
		o[0] = c->id, o[1] = c->cell.face, o[2] = c->cell.level, o[3] = c->cell.i, o[4] = c->cell.j;
		o[5] = c->kind, o[6] = c->idx, o[7] = c->state;
		o[8] = c->pos.x, o[9] = c->pos.y, o[10] = c->pos.z;
		o[11] = c->vel.x, o[12] = c->vel.y, o[13] = c->vel.z;
		o[14] = c->fwd.x, o[15] = c->fwd.y, o[16] = c->fwd.z;
		o[17] = L->hp > 0 ? c->hp / L->hp : 0;
		o[18] = h->world->t - c->t_state;
		o[19] = c->size;
	}
	int k = 2 + wd->n * WPP, m = 0;
	for (int i = 0; i < wd->ndead; i++)
		m += wd->dead[i].body == wd->body;
	d[k++] = m;
	for (int i = 0; i < wd->ndead; i++)
		if (wd->dead[i].body == wd->body) {
			const trs_wild_dead *x = &wd->dead[i];
			d[k++] = x->cell.face, d[k++] = x->cell.level, d[k++] = x->cell.i, d[k++] = x->cell.j, d[k++] = x->kind, d[k++] = x->idx;
		}
	ret_doubles(r, d, k);
	free(d);
}

#define LPP 11
/* lasers(origin) -> per laser on any vessel near the origin on its body:
 * vessel id, part uid, mount (3) and where it points (3, unit) relative to
 * the origin (SOI axes), beam length (0: not firing), what it hit (a
 * trs_thing_kind), 1 if it's the Jaegers': LPP doubles each */
static void m_lasers(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!ok_vessel(h, a[0].i)) {
		ret_doubles(r, NULL, 0);
		return;
	}
	trs_world *w = h->world;
	int origin = (int)a[0].i;
	trs_v3 ro = origin_r(w, origin);
	int cap = 64, n = 0;
	double *d = malloc(sizeof(double) * (size_t)(cap * LPP));
	for (int id = 0; id < w->nvessels; id++) {
		const trs_vessel *vs = &w->vessels[id];
		if (!vs->alive || !vs->craft || !vs->nlasers || vs->body != w->vessels[origin].body)
			continue;
		if (vs->mode != TRS_MODE_PHYSICS && vs->mode != TRS_MODE_LANDED)
			continue;
		trs_v3 vr, vv;
		trs_vessel_state(w, id, &vr, &vv);
		if (v3_len(v3_sub(vr, ro)) > 30000)
			continue;
		trs_quat q = vs->mode == TRS_MODE_LANDED
			? quat_normalize(quat_mul(trs_body_quat(&w->sys.bodies[vs->body], w->t), vs->fixed_q)) : vs->q;
		for (int i = 0; i < vs->craft->count; i++) {
			const trs_part *p = &vs->craft->parts[i];
			if (trs_craft_def(vs->craft, i)->laser_range <= 0)
				continue;
			if (n >= cap) {
				cap *= 2;
				d = realloc(d, sizeof(double) * (size_t)(cap * LPP));
			}
			trs_v3 at, axis;
			trs_laser_mount(w, vs, i, &at, &axis);
			trs_v3 aim = v3_len2(p->aim) > 0.5 ? v3_norm(quat_rotate(q, p->aim)) : axis;
			at = v3_sub(at, ro);
			double *o = d + n * LPP;
			o[0] = id, o[1] = p->uid;
			o[2] = at.x, o[3] = at.y, o[4] = at.z;
			o[5] = aim.x, o[6] = aim.y, o[7] = aim.z;
			o[8] = p->beam, o[9] = p->beam_hit, o[10] = vs->is_alien;
			n++;
		}
	}
	ret_doubles(r, d, (int64_t)n * LPP);
	free(d);
}

/* set_guard(id, mode 0..2) -> lasers aboard */
static void m_set_guard(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = h->world ? trs_world_set_guard(h->world, (int)a[0].i, (int)a[1].i) : 0;
}

/* lock_on(id, kind, thing id, serial) -> lasers aboard (kind 0 lets go) */
static void m_lock_on(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_thing th = { (int)a[1].i, (int)a[2].i, (int)a[3].i };
	r->i = h->world ? trs_world_lock(h->world, (int)a[0].i, th) : 0;
}

/* hand_fire(id, origin, on, point rel. origin) -> lasers aboard */
static void m_hand_fire(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->i = 0;
	if (!ok_vessel(h, a[0].i) || !ok_vessel(h, a[1].i) || a[3].arr.n < 3)
		return;
	trs_v3 p = v3_add(origin_r(h->world, (int)a[1].i), v3(a[3].arr.data[0], a[3].arr.data[1], a[3].arr.data[2]));
	r->i = trs_world_hand_fire(h->world, (int)a[0].i, a[2].b, p);
}

/* pick(origin, from rel. origin, unit dir, range) -> [kind, id, serial,
 * distance, part index]: what a ray (the camera's, through the cursor)
 * meets first; nothing: kind 0 and distance = range */
static void m_pick(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	double d[5] = { 0, -1, 0, a[3].f, -1 };
	if (ok_vessel(h, a[0].i) && a[1].arr.n >= 3 && a[2].arr.n >= 3) {
		trs_world *w = h->world;
		int origin = (int)a[0].i;
		trs_v3 o = v3_add(origin_r(w, origin), v3(a[1].arr.data[0], a[1].arr.data[1], a[1].arr.data[2]));
		trs_v3 dir = v3_norm(v3(a[2].arr.data[0], a[2].arr.data[1], a[2].arr.data[2]));
		double dist;
		int part;
		trs_thing th = trs_world_ray(w, w->vessels[origin].body, o, dir, a[3].f, -1, &dist, &part);
		d[0] = th.kind, d[1] = th.id, d[2] = th.serial, d[3] = dist, d[4] = part;
	}
	ret_doubles(r, d, 5);
}

/* one thing, as the scripts and the HUD see it, from vessel `from` */
static trs_json *thing_json(const trs_world *w, int from, trs_thing th) {
	const trs_vessel *me = &w->vessels[from];
	const trs_body *b = &w->sys.bodies[me->body];
	trs_v3 at, mine = origin_r(w, from);
	double rad;
	if (!trs_thing_where(w, me->body, th, &at, &rad))
		return NULL;
	trs_json *o = trs_json_new(TRS_JSON_OBJECT);
	trs_json_set(o, "kind", trs_json_new_num(th.kind));
	trs_json_set(o, "id", trs_json_new_num(th.id));
	trs_json_set(o, "serial", trs_json_new_num(th.serial));
	trs_v3 d = v3_sub(at, mine), up, n, e;
	trs_local_frame(mine, &up, &n, &e);
	double bearing = atan2(v3_dot(d, e), v3_dot(d, n)) * 180 / TRS_PI;
	if (bearing < 0)
		bearing += 360;
	trs_json_set(o, "distance", trs_json_new_num(fmax(0, v3_len(d) - rad)));
	trs_json_set(o, "bearing", trs_json_new_num(bearing));
	trs_json_set(o, "above", trs_json_new_num(v3_dot(d, up)));
	trs_json *rel = trs_json_new(TRS_JSON_ARRAY);
	trs_json_push(rel, trs_json_new_num(d.x));
	trs_json_push(rel, trs_json_new_num(d.y));
	trs_json_push(rel, trs_json_new_num(d.z));
	trs_json_set(o, "rel", rel);
	trs_json_set(o, "radius", trs_json_new_num(rad));
	trs_json_set(o, "threat", trs_json_new_bool(trs_thing_threat(w, from, th, 1)));
	trs_json_set(o, "in_reach", trs_json_new_bool(trs_laser_reaches(w, from, th)));
	if (th.kind == TRS_THING_CRITTER) {
		const trs_critter *c = &w->wild.c[trs_wild_find(w, th.id)];
		const trs_life *L = trs_critter_kind(w, c);
		trs_json_set(o, "what", trs_json_new_str("animal"));
		trs_json_set(o, "species", trs_json_new_str(L->id));
		trs_json_set(o, "temper", trs_json_new_str(TEMPERS[L->temper]));
		trs_json_set(o, "hostile", trs_json_new_bool(L->temper == TRS_TEMPER_HOSTILE));
		trs_json_set(o, "penguin", trs_json_new_bool(L->penguin));
		trs_json_set(o, "alien", trs_json_new_bool(0));
		trs_json_set(o, "health", trs_json_new_num(100.0 * c->hp / fmax(L->hp, 1e-9)));
		trs_json_set(o, "state", trs_json_new_str(STATES[c->state]));
		trs_json_set(o, "size", trs_json_new_num(c->size));
		int after = c->other.kind == TRS_THING_VESSEL && c->other.id == from && c->state == TRS_CRIT_CHARGE;
		trs_json_set(o, "after_us", trs_json_new_bool(after));
	} else {
		const trs_vessel *vs = &w->vessels[th.id];
		double worst = 0;
		for (int i = 0; i < vs->craft->count; i++)
			if (trs_craft_def(vs->craft, i)->alien_pilot || trs_vessel_is_eva(vs) || i == 0)
				worst = fmax(worst, vs->craft->parts[i].damage / fmax(trs_craft_def(vs->craft, i)->hp, 1e-9));
		trs_json_set(o, "what", trs_json_new_str(trs_vessel_is_eva(vs) ? "penguin" : "ship"));
		trs_json_set(o, "name", trs_json_new_str(vs->craft->name));
		trs_json_set(o, "hostile", trs_json_new_bool(vs->is_alien != me->is_alien));
		trs_json_set(o, "penguin", trs_json_new_bool(trs_vessel_is_eva(vs)));
		trs_json_set(o, "alien", trs_json_new_bool(vs->is_alien));
		trs_json_set(o, "health", trs_json_new_num(fmax(0, 100.0 * (1 - worst))));
		trs_json_set(o, "state", trs_json_new_str(vs->pilot ? (vs->pilot_state ? "attacking" : "patrolling") : (vs->landed ? "landed" : "flying")));
		trs_json_set(o, "after_us", trs_json_new_bool(vs->pilot && vs->pilot_target.id == from && vs->pilot_state));
	}
	(void)b;
	return o;
}

/* thing_info(from, kind, id, serial) -> JSON (see things_near), {} if gone */
static void m_thing_info(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *o = NULL;
	if (ok_vessel(h, a[0].i))
		o = thing_json(h->world, (int)a[0].i, (trs_thing){ (int)a[1].i, (int)a[2].i, (int)a[3].i });
	ret_json(r, o ? o : trs_json_new(TRS_JSON_OBJECT));
}

static int by_dist(const void *x, const void *y) {
	double a = ((const double *)x)[0], b = ((const double *)y)[0];
	return a < b ? -1 : a > b;
}

/* things_near(from, range) -> JSON [{kind, id, serial, what (animal,
 * ship, penguin), species or name, distance (m, to its edge), bearing (deg
 * from north), above (m), rel (3, SOI axes), hostile, penguin, alien,
 * threat, health (%), state, after_us}], nearest first: everything alive
 * near vessel `from` on its body but itself */
static void m_things_near(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	if (!ok_vessel(h, a[0].i)) {
		ret_json(r, arr);
		return;
	}
	trs_world *w = h->world;
	int from = (int)a[0].i;
	const trs_vessel *me = &w->vessels[from];
	trs_v3 mine = origin_r(w, from);
	double (*list)[4] = malloc(sizeof(double[4]) * (TRS_WILD_MAX + TRS_MAX_VESSELS));
	int n = 0;
	for (int i = 0; i < w->nvessels; i++) {
		trs_thing th = { TRS_THING_VESSEL, i, w->vessels[i].serial };
		trs_v3 at;
		double rad;
		if (i == from || !w->vessels[i].alive || !w->vessels[i].craft || !trs_thing_where(w, me->body, th, &at, &rad))
			continue;
		double d = v3_len(v3_sub(at, mine)) - rad;
		if (d <= a[1].f)
			list[n][0] = d, list[n][1] = th.kind, list[n][2] = th.id, list[n][3] = th.serial, n++;
	}
	if (w->wild.body == me->body)
		for (int i = 0; i < w->wild.n; i++) {
			trs_thing th = { TRS_THING_CRITTER, w->wild.c[i].id, 0 };
			trs_v3 at;
			double rad;
			if (!trs_thing_where(w, me->body, th, &at, &rad))
				continue;
			double d = v3_len(v3_sub(at, mine)) - rad;
			if (d <= a[1].f)
				list[n][0] = d, list[n][1] = th.kind, list[n][2] = th.id, list[n][3] = 0, n++;
		}
	qsort(list, (size_t)n, sizeof list[0], by_dist);
	for (int k = 0; k < n; k++) {
		trs_json *o = thing_json(w, from, (trs_thing){ (int)list[k][1], (int)list[k][2], (int)list[k][3] });
		if (o)
			trs_json_push(arr, o);
	}
	free(list);
	ret_json(r, arr);
}

/* nest_craft(craft_id, craft_text) -> ok: the ship a nest puts up */
static void m_nest_craft(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	r->b = 0;
	if (!h->world || !h->catalog_loaded)
		return;
	trs_json *j = trs_json_parse(a[1].s, h->err, sizeof h->err);
	if (!j)
		return;
	trs_craft c;
	if (trs_craft_from_json(&c, &h->catalog, j, h->err, sizeof h->err)) {
		trs_world_set_nest_craft(h->world, a[0].s, &c);
		trs_craft_free(&c);
		r->b = 1;
	}
	trs_json_free(j);
}

/* nests(body) -> JSON [{id, name, craft, lat, lon, count, lost, up, detect, patrol}] */
static void m_nests(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_json *arr = trs_json_new(TRS_JSON_ARRAY);
	if (h->world && a[0].i >= 0 && a[0].i < h->world->sys.count) {
		const trs_world *w = h->world;
		const trs_body *b = &w->sys.bodies[a[0].i];
		for (int k = 0; k < b->nnests; k++) {
			const trs_nest *ne = &b->nests[k];
			trs_json *o = trs_json_new(TRS_JSON_OBJECT);
			trs_json_set(o, "id", trs_json_new_str(ne->id));
			trs_json_set(o, "name", trs_json_new_str(ne->name));
			trs_json_set(o, "craft", trs_json_new_str(ne->craft));
			trs_json_set(o, "lat", trs_json_new_num(asin(ne->dir.z) * 180 / TRS_PI));
			trs_json_set(o, "lon", trs_json_new_num(atan2(ne->dir.y, ne->dir.x) * 180 / TRS_PI));
			trs_json_set(o, "count", trs_json_new_num(ne->count));
			trs_json_set(o, "lost", trs_json_new_num(w->nest_lost[a[0].i][k]));
			int up = 0, code = (int)a[0].i * TRS_MAX_NESTS + k + 1;
			for (int i = 0; i < w->nvessels; i++)
				up += w->vessels[i].alive && w->vessels[i].pilot == code;
			trs_json_set(o, "up", trs_json_new_num(up));
			trs_json_set(o, "detect", trs_json_new_num(ne->detect));
			trs_json_set(o, "patrol", trs_json_new_num(ne->patrol));
			trs_json_push(arr, o);
		}
	}
	ret_json(r, arr);
}

/* find_life(body, species id, seed) -> [lat_deg, lon_deg] where one of
 * that kind is now (on its loop), or [] if none was found */
static void m_find_life(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (!h->world || a[0].i < 0 || a[0].i >= h->world->sys.count) {
		ret_doubles(r, NULL, 0);
		return;
	}
	const trs_body *b = &h->world->sys.bodies[a[0].i];
	int k = -1;
	for (int q = 0; q < b->nlife; q++)
		if (!strcmp(b->life[q].id, a[1].s))
			k = q;
	if (k < 0) {
		ret_doubles(r, NULL, 0);
		return;
	}
	unsigned s = (unsigned)a[2].i * 2654435761u + 1u;
	int level = trs_life_level(b), max = 2000;
	trs_life_instance *li = malloc(sizeof(trs_life_instance) * (size_t)max);
	double out[2];
	int found = 0;
	for (int q = 0; q < 20000 && !found; q++) {
		s = s * 1664525u + 1013904223u;
		double z = (s >> 8) / 16777216.0 * 2 - 1;
		s = s * 1664525u + 1013904223u;
		double an = (s >> 8) / 16777216.0 * 2 * TRS_PI, rr = sqrt(1 - z * z);
		trs_patch_id p = trs_patch_from_dir(v3(rr * cos(an), rr * sin(an), z), level);
		trs_v3 c, pos, fwd;
		if (trs_life_cell(b, p, 1u << k, &c, li, max) > 0) {
			trs_life_animal_at(&b->life[k], &li[0], c, h->world->t, &pos, &fwd);
			trs_v3 d = v3_norm(pos);
			out[0] = asin(d.z) * 180 / TRS_PI, out[1] = atan2(d.y, d.x) * 180 / TRS_PI;
			found = 1;
		}
	}
	free(li);
	ret_doubles(r, out, found ? 2 : 0);
}

#define V TRS_T_VOID
#define I TRS_T_INT
#define F TRS_T_FLOAT
#define S TRS_T_STRING
#define A TRS_T_F64ARR
#define B TRS_T_BOOL

const trs_method_def TRS_SIM_COMBAT_METHODS[] = {
	{ "set_focus", m_set_focus, V, 1, { I }, { "id" } },
	{ "wild_list", m_wild_list, A, 0, { 0 }, { 0 } },
	{ "lasers", m_lasers, A, 1, { I }, { "origin" } },
	{ "set_guard", m_set_guard, I, 2, { I, I }, { "id", "mode" } },
	{ "lock_on", m_lock_on, I, 4, { I, I, I, I }, { "id", "kind", "thing", "serial" } },
	{ "hand_fire", m_hand_fire, I, 4, { I, I, B, A }, { "id", "origin", "on", "point" } },
	{ "pick", m_pick, A, 4, { I, A, A, F }, { "origin", "from", "dir", "range" } },
	{ "thing_info", m_thing_info, S, 4, { I, I, I, I }, { "from", "kind", "thing", "serial" } },
	{ "things_near", m_things_near, S, 2, { I, F }, { "from", "range" } },
	{ "nest_craft", m_nest_craft, B, 2, { S, S }, { "craft_id", "text" } },
	{ "nests", m_nests, S, 1, { I }, { "body" } },
	{ "find_life", m_find_life, A, 3, { I, S, I }, { "body", "species", "seed" } },
};
const int TRS_SIM_COMBAT_METHOD_COUNT = sizeof(TRS_SIM_COMBAT_METHODS) / sizeof(TRS_SIM_COMBAT_METHODS[0]);
