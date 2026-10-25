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

/* Saving and restoring a whole world: every vessel's raw state and every
 * part's runtime state, so a restored world steps on exactly as the saved
 * one would have (caches are rebuilt from the same inputs). Doubles go out
 * as the shortest decimal that round-trips; -0, infinities and NaN as
 * strings so nothing is lost. */

#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static trs_json *dnum(double v) {
	if (isnan(v))
		return trs_json_new_str("nan");
	if (isinf(v))
		return trs_json_new_str(v > 0 ? "inf" : "-inf");
	if (v == 0 && signbit(v))
		return trs_json_new_str("-0");
	return trs_json_new_num(v);
}

static double dval(const trs_json *j, double fallback) {
	if (!j)
		return fallback;
	if (j->type == TRS_JSON_NUMBER)
		return j->num;
	if (j->type == TRS_JSON_STRING) {
		if (!strcmp(j->str, "nan"))
			return NAN;
		if (!strcmp(j->str, "inf"))
			return INFINITY;
		if (!strcmp(j->str, "-inf"))
			return -INFINITY;
		if (!strcmp(j->str, "-0"))
			return -0.0;
	}
	return fallback;
}

static trs_json *darr(const double *d, int n) {
	trs_json *a = trs_json_new(TRS_JSON_ARRAY);
	for (int i = 0; i < n; i++)
		trs_json_push(a, dnum(d[i]));
	return a;
}

static trs_json *v3j(trs_v3 v) {
	double d[3] = { v.x, v.y, v.z };
	return darr(d, 3);
}

static trs_json *qj(trs_quat q) {
	double d[4] = { q.x, q.y, q.z, q.w };
	return darr(d, 4);
}

static double get_d(const trs_json *o, const char *k) {
	return dval(trs_json_get(o, k), 0);
}

static trs_v3 get_v3(const trs_json *o, const char *k) {
	const trs_json *a = trs_json_get(o, k);
	return v3(dval(trs_json_at(a, 0), 0), dval(trs_json_at(a, 1), 0), dval(trs_json_at(a, 2), 0));
}

static trs_quat get_q(const trs_json *o, const char *k) {
	const trs_json *a = trs_json_get(o, k);
	trs_quat q = { dval(trs_json_at(a, 0), 0), dval(trs_json_at(a, 1), 0), dval(trs_json_at(a, 2), 0), dval(trs_json_at(a, 3), 1) };
	return q;
}

#define PUT_D(o, s, f) trs_json_set(o, #f, dnum((s)->f))
#define PUT_I(o, s, f) trs_json_set(o, #f, trs_json_new_num((s)->f))
#define PUT_V(o, s, f) trs_json_set(o, #f, v3j((s)->f))
#define PUT_Q(o, s, f) trs_json_set(o, #f, qj((s)->f))
#define GET_D(o, s, f) ((s)->f = get_d(o, #f))
#define GET_I(o, s, f) ((s)->f = (int)trs_json_num(o, #f, 0))
#define GET_V(o, s, f) ((s)->f = get_v3(o, #f))
#define GET_Q(o, s, f) ((s)->f = get_q(o, #f))

static trs_json *craft_json(const trs_craft *c) {
	trs_json *cj = trs_json_new(TRS_JSON_OBJECT);
	trs_json_set(cj, "name", trs_json_new_str(c->name));
	trs_json_set(cj, "description", trs_json_new_str(c->description));
	trs_json_set(cj, "next_uid", trs_json_new_num(c->next_uid));
	trs_json *parts = trs_json_new(TRS_JSON_ARRAY);
	for (int i = 0; i < c->count; i++) {
		const trs_part *p = &c->parts[i];
		trs_json *pj = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(pj, "part", trs_json_new_str(trs_craft_def(c, i)->id));
		PUT_I(pj, p, uid);
		PUT_I(pj, p, parent);
		trs_json_set(pj, "node", trs_json_new_str(p->node));
		trs_json_set(pj, "parent_node", trs_json_new_str(p->parent_node));
		PUT_V(pj, p, pos);
		PUT_Q(pj, p, rot);
		PUT_I(pj, p, stage);
		PUT_I(pj, p, sym);
		PUT_D(pj, p, fuel);
		PUT_D(pj, p, mono);
		PUT_D(pj, p, limit);
		PUT_I(pj, p, engine_on);
		PUT_I(pj, p, chute);
		PUT_I(pj, p, legs_deployed);
		PUT_I(pj, p, fired);
		PUT_D(pj, p, temp);
		PUT_D(pj, p, chute_cda);
		trs_json_set(pj, "rcs_fire", darr(p->rcs_fire, TRS_MAX_NOZZLES));
		trs_json_push(parts, pj);
	}
	trs_json_set(cj, "parts", parts);
	return cj;
}

static int craft_load(trs_craft *c, const trs_part_catalog *cat, const trs_json *cj, char *err, size_t errlen) {
	trs_craft_init(c, cat);
	snprintf(c->name, sizeof c->name, "%s", trs_json_str(cj, "name", ""));
	snprintf(c->description, sizeof c->description, "%s", trs_json_str(cj, "description", ""));
	const trs_json *parts = trs_json_get(cj, "parts");
	for (size_t i = 0; parts && i < parts->count; i++) {
		const trs_json *pj = trs_json_at(parts, i);
		int def = trs_catalog_find(cat, trs_json_str(pj, "part", ""));
		if (def < 0) {
			snprintf(err, errlen, "saved world: unknown part '%s'", trs_json_str(pj, "part", ""));
			trs_craft_free(c);
			return 0;
		}
		int k = trs_craft_add(c, def, (int)trs_json_num(pj, "parent", -1), trs_json_str(pj, "node", ""),
				trs_json_str(pj, "parent_node", ""), get_v3(pj, "pos"), get_q(pj, "rot"), (int)trs_json_num(pj, "stage", -1),
				(int)trs_json_num(pj, "sym", 0));
		trs_part *p = &c->parts[k];
		GET_I(pj, p, uid);
		GET_D(pj, p, fuel);
		GET_D(pj, p, mono);
		GET_D(pj, p, limit);
		GET_I(pj, p, engine_on);
		GET_I(pj, p, chute);
		GET_I(pj, p, legs_deployed);
		GET_I(pj, p, fired);
		GET_D(pj, p, temp);
		GET_D(pj, p, chute_cda);
		const trs_json *rf = trs_json_get(pj, "rcs_fire");
		for (int n = 0; n < TRS_MAX_NOZZLES; n++)
			p->rcs_fire[n] = dval(trs_json_at(rf, (size_t)n), 0);
	}
	c->next_uid = (int)trs_json_num(cj, "next_uid", c->count);
	return 1;
}

trs_json *trs_world_to_json(const trs_world *w) {
	trs_json *j = trs_json_new(TRS_JSON_OBJECT);
	trs_json_set(j, "format", trs_json_new_str("trs-world"));
	trs_json_set(j, "version", trs_json_new_num(1));
	PUT_D(j, w, t);
	PUT_D(j, w, t_comp);
	PUT_I(j, w, next_uid);
	trs_json *vl = trs_json_new(TRS_JSON_ARRAY);
	for (int id = 0; id < w->nvessels; id++) {
		const trs_vessel *vs = &w->vessels[id];
		if (!vs->alive)
			continue;
		trs_json *o = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(o, "id", trs_json_new_num(id));
		trs_json_set(o, "body_id", trs_json_new_str(w->sys.bodies[vs->body].id));
		PUT_I(o, vs, mode);
		PUT_V(o, vs, r);
		PUT_V(o, vs, v);
		if (vs->mode == TRS_MODE_RAILS) {
			const trs_orbit *ob = &vs->orbit;
			trs_json *oj = trs_json_new(TRS_JSON_OBJECT);
			PUT_D(oj, ob, mu);
			PUT_V(oj, ob, r0);
			PUT_V(oj, ob, v0);
			PUT_D(oj, ob, t0);
			trs_json_set(o, "orbit", oj);
		}
		PUT_I(o, vs, soi_changes);
		PUT_Q(o, vs, q);
		PUT_V(o, vs, w);
		PUT_D(o, vs, throttle);
		PUT_V(o, vs, input);
		PUT_I(o, vs, sas);
		PUT_Q(o, vs, sas_hold);
		PUT_V(o, vs, sas_target);
		PUT_I(o, vs, next_stage);
		PUT_I(o, vs, rcs);
		PUT_V(o, vs, translate);
		PUT_V(o, vs, rcs_rot);
		PUT_V(o, vs, rcs_force);
		PUT_D(o, vs, rcs_mdot);
		PUT_D(o, vs, rcs_dv_expended);
		PUT_V(o, vs, sensed_accel);
		PUT_D(o, vs, dv_expended);
		PUT_D(o, vs, pressure);
		PUT_D(o, vs, density);
		PUT_D(o, vs, altitude);
		PUT_V(o, vs, fixed_r);
		PUT_Q(o, vs, fixed_q);
		PUT_I(o, vs, clamped);
		PUT_I(o, vs, contacts);
		PUT_I(o, vs, landed);
		PUT_D(o, vs, max_impact);
		PUT_V(o, vs, ext_force);
		PUT_V(o, vs, ext_torque);
		PUT_I(o, vs, fresh);
		trs_json *gh = trs_json_new(TRS_JSON_ARRAY);
		for (int k = 0; k < vs->nghost; k++) {
			trs_json *g = trs_json_new(TRS_JSON_ARRAY);
			trs_json_push(g, trs_json_new_num(vs->ghost[k]));
			trs_json_push(g, dnum(vs->ghost_until[k]));
			trs_json_push(gh, g);
		}
		trs_json_set(o, "ghosts", gh);
		trs_json *pe = trs_json_new(TRS_JSON_ARRAY);
		for (int k = 0; k < vs->npending; k++) {
			trs_json *g = trs_json_new(TRS_JSON_ARRAY);
			trs_json_push(g, trs_json_new_num(vs->pending_uid[k]));
			trs_json_push(g, dnum(vs->pending_speed[k]));
			trs_json_push(pe, g);
		}
		trs_json_set(o, "pending", pe);
		if (vs->craft)
			trs_json_set(o, "craft", craft_json(vs->craft));
		trs_json_push(vl, o);
	}
	trs_json_set(j, "vessels", vl);
	return j;
}

int trs_world_from_json(trs_world *w, const trs_part_catalog *cat, const trs_json *j, char *err, size_t errlen) {
	if (strcmp(trs_json_str(j, "format", ""), "trs-world") != 0) {
		snprintf(err, errlen, "not a saved world");
		return 0;
	}
	const trs_json *vl = trs_json_get(j, "vessels");
	/* parse every craft first, so a bad save leaves the world untouched */
	size_t n = vl ? vl->count : 0;
	trs_craft *crafts = calloc(n ? n : 1, sizeof(trs_craft));
	int *bodies = calloc(n ? n : 1, sizeof(int));
	for (size_t i = 0; i < n; i++) {
		const trs_json *o = trs_json_at(vl, i);
		int id = (int)trs_json_num(o, "id", -1);
		bodies[i] = trs_system_find(&w->sys, trs_json_str(o, "body_id", ""));
		int ok = id >= 0 && id < TRS_MAX_VESSELS && bodies[i] >= 0;
		if (!ok)
			snprintf(err, errlen, "saved world: vessel %zu has a bad id or body", i);
		if (ok && trs_json_get(o, "craft"))
			ok = craft_load(&crafts[i], cat, trs_json_get(o, "craft"), err, errlen);
		if (!ok) {
			for (size_t k = 0; k < i; k++)
				if (crafts[k].cat)
					trs_craft_free(&crafts[k]);
			free(crafts);
			free(bodies);
			return 0;
		}
	}
	trs_world_free(w);
	memset(w->vessels, 0, sizeof w->vessels);
	w->nevents = 0;
	GET_D(j, w, t);
	GET_D(j, w, t_comp);
	GET_I(j, w, next_uid);
	for (size_t i = 0; i < n; i++) {
		const trs_json *o = trs_json_at(vl, i);
		int id = (int)trs_json_num(o, "id", -1);
		trs_vessel *vs = &w->vessels[id];
		if (id >= w->nvessels)
			w->nvessels = id + 1;
		vs->alive = 1;
		vs->body = bodies[i];
		GET_I(o, vs, mode);
		GET_V(o, vs, r);
		GET_V(o, vs, v);
		const trs_json *oj = trs_json_get(o, "orbit");
		if (oj)
			trs_orbit_from_state(&vs->orbit, get_d(oj, "mu"), get_v3(oj, "r0"), get_v3(oj, "v0"), get_d(oj, "t0"));
		GET_I(o, vs, soi_changes);
		GET_Q(o, vs, q);
		GET_V(o, vs, w);
		GET_D(o, vs, throttle);
		GET_V(o, vs, input);
		GET_I(o, vs, sas);
		GET_Q(o, vs, sas_hold);
		GET_V(o, vs, sas_target);
		GET_I(o, vs, next_stage);
		GET_I(o, vs, rcs);
		GET_V(o, vs, translate);
		GET_V(o, vs, rcs_rot);
		GET_V(o, vs, rcs_force);
		GET_D(o, vs, rcs_mdot);
		GET_D(o, vs, rcs_dv_expended);
		GET_V(o, vs, sensed_accel);
		GET_D(o, vs, dv_expended);
		GET_D(o, vs, pressure);
		GET_D(o, vs, density);
		GET_D(o, vs, altitude);
		GET_V(o, vs, fixed_r);
		GET_Q(o, vs, fixed_q);
		GET_I(o, vs, clamped);
		GET_I(o, vs, contacts);
		GET_I(o, vs, landed);
		GET_D(o, vs, max_impact);
		GET_V(o, vs, ext_force);
		GET_V(o, vs, ext_torque);
		GET_I(o, vs, fresh);
		const trs_json *gh = trs_json_get(o, "ghosts");
		for (size_t k = 0; gh && k < gh->count && vs->nghost < 16; k++) {
			vs->ghost[vs->nghost] = (int)trs_json_at(trs_json_at(gh, k), 0)->num;
			vs->ghost_until[vs->nghost++] = dval(trs_json_at(trs_json_at(gh, k), 1), 0);
		}
		const trs_json *pe = trs_json_get(o, "pending");
		for (size_t k = 0; pe && k < pe->count && vs->npending < TRS_MAX_PENDING; k++) {
			vs->pending_uid[vs->npending] = (int)trs_json_at(trs_json_at(pe, k), 0)->num;
			vs->pending_speed[vs->npending++] = dval(trs_json_at(trs_json_at(pe, k), 1), 0);
		}
		if (crafts[i].cat) {
			vs->craft = malloc(sizeof(trs_craft));
			*vs->craft = crafts[i];
			trs_vessel_refresh(w, id);
		} else {
			vs->mass = 1.0;
		}
	}
	free(crafts);
	free(bodies);
	return 1;
}
