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

/* TrsSim craft API: part catalog, hangar readouts, canonical save text. */
#include "sim.h"
#include "trs/craft.h"
#include "trs/json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static trs_sim_handle *H(void *self) { return self; }

/* load_catalog(path) -> "" or error */
static void m_load_catalog(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	if (h->catalog_loaded)
		trs_catalog_free(&h->catalog);
	h->catalog_loaded = trs_catalog_load(&h->catalog, a[0].s, h->err, sizeof h->err);
	r->s = trs_strdup_printf("%s", h->catalog_loaded ? "" : h->err);
}

static int parse_craft(trs_sim_handle *h, const char *text, trs_craft *c, char *err, size_t errlen) {
	if (!h->catalog_loaded) {
		snprintf(err, errlen, "no part catalog loaded");
		return 0;
	}
	trs_json *j = trs_json_parse(text, err, errlen);
	if (!j)
		return 0;
	int ok = trs_craft_from_json(c, &h->catalog, j, err, errlen);
	trs_json_free(j);
	return ok;
}

/* ---- binary-safe craft transfer ------------------------------------------
 * Godot's float<->text conversions are not correctly rounded, so craft
 * geometry crosses into GDScript only as doubles: a "meta" JSON with the
 * structure (strings/ints) plus a PackedFloat64Array with 9 doubles per
 * part: pos xyz, rot xyzw, fuel (-1 = full), thrust limit. trs_core owns
 * all text. */

#define FPP 9

static void ret_floats(trs_val *r, const trs_craft *c) {
	r->arr.n = (int64_t)c->count * FPP;
	r->arr.data = malloc(sizeof(double) * (size_t)(r->arr.n ? r->arr.n : 1));
	for (int i = 0; i < c->count; i++) {
		const trs_part *p = &c->parts[i];
		const trs_part_def *d = trs_craft_def(c, i);
		double cap = d->fuel_liquid > 0 ? d->fuel_liquid : d->fuel_solid;
		double *o = r->arr.data + (size_t)i * FPP;
		o[0] = p->pos.x, o[1] = p->pos.y, o[2] = p->pos.z;
		o[3] = p->rot.x, o[4] = p->rot.y, o[5] = p->rot.z, o[6] = p->rot.w;
		o[7] = p->fuel == cap ? -1 : p->fuel;
		o[8] = p->limit;
	}
}

static trs_json *meta_of(const trs_craft *c) {
	trs_json *m = trs_json_new(TRS_JSON_OBJECT);
	trs_json_set(m, "name", trs_json_new_str(c->name));
	trs_json_set(m, "description", trs_json_new_str(c->description));
	trs_json *parts = trs_json_new(TRS_JSON_ARRAY);
	for (int i = 0; i < c->count; i++) {
		const trs_part *p = &c->parts[i];
		trs_json *pj = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(pj, "part", trs_json_new_str(trs_craft_def(c, i)->id));
		trs_json_set(pj, "parent", trs_json_new_num(p->parent));
		trs_json_set(pj, "node", trs_json_new_str(p->node));
		trs_json_set(pj, "parent_node", trs_json_new_str(p->parent_node));
		trs_json_set(pj, "stage", trs_json_new_num(p->stage));
		trs_json_set(pj, "sym", trs_json_new_num(p->sym));
		trs_json_set(pj, "groups", trs_json_new_num(p->groups));
		trs_json_push(parts, pj);
	}
	trs_json_set(m, "parts", parts);
	return m;
}

/* Builds a craft from meta JSON + floats. */
static int craft_from_bin(trs_sim_handle *h, const char *meta, const trs_val *floats, trs_craft *c, char *err,
		size_t errlen) {
	if (!h->catalog_loaded) {
		snprintf(err, errlen, "no part catalog loaded");
		return 0;
	}
	trs_json *m = trs_json_parse(meta, err, errlen);
	if (!m)
		return 0;
	const trs_json *parts = trs_json_get(m, "parts");
	int n = parts && parts->type == TRS_JSON_ARRAY ? (int)parts->count : 0;
	if (n == 0 || floats->arr.n != (int64_t)n * FPP) {
		snprintf(err, errlen, "craft needs parts and %d floats per part", FPP);
		trs_json_free(m);
		return 0;
	}
	trs_craft_init(c, &h->catalog);
	snprintf(c->name, sizeof c->name, "%s", trs_json_str(m, "name", "Untitled"));
	snprintf(c->description, sizeof c->description, "%s", trs_json_str(m, "description", ""));
	for (int i = 0; i < n; i++) {
		const trs_json *pj = trs_json_at(parts, (size_t)i);
		int def = trs_catalog_find(&h->catalog, trs_json_str(pj, "part", ""));
		int parent = (int)trs_json_num(pj, "parent", -1);
		if (def < 0 || (i == 0) != (parent < 0) || parent >= i) {
			snprintf(err, errlen, "part %d: unknown part or bad parent", i);
			trs_craft_free(c);
			trs_json_free(m);
			return 0;
		}
		const double *f = floats->arr.data + (size_t)i * FPP;
		trs_quat q = { f[3], f[4], f[5], f[6] };
		int k = trs_craft_add(c, def, parent, trs_json_str(pj, "node", ""), trs_json_str(pj, "parent_node", ""),
				v3(f[0], f[1], f[2]), q, (int)trs_json_num(pj, "stage", -1), (int)trs_json_num(pj, "sym", 0));
		if (f[7] >= 0)
			c->parts[k].fuel = f[7];
		c->parts[k].groups = (int)trs_json_num(pj, "groups", 0) & 0x1ff;
		if (!(f[8] > 0 && f[8] <= 1)) {
			snprintf(err, errlen, "part %d: thrust limit must be in (0, 1]", i);
			trs_craft_free(c);
			trs_json_free(m);
			return 0;
		}
		c->parts[k].limit = f[8];
	}
	trs_json_free(m);
	return 1;
}

/* craft_decode(text) -> meta JSON ({"error"} on failure) */
static void m_craft_decode(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_craft c;
	char err[256];
	if (!parse_craft(h, a[0].s, &c, err, sizeof err)) {
		trs_json *e = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(e, "error", trs_json_new_str(err));
		r->s = trs_json_write_compact(e);
		trs_json_free(e);
		return;
	}
	trs_json *m = meta_of(&c);
	r->s = trs_json_write_compact(m);
	trs_json_free(m);
	trs_craft_free(&c);
}

/* craft_decode_floats(text) -> PackedFloat64Array (8 per part) */
static void m_craft_decode_floats(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_craft c;
	char err[256];
	if (!parse_craft(h, a[0].s, &c, err, sizeof err)) {
		r->arr.n = 0;
		r->arr.data = malloc(sizeof(double));
		return;
	}
	ret_floats(r, &c);
	trs_craft_free(&c);
}

/* craft_encode(meta, floats) -> canonical save text ("" on error, see last_error) */
static void m_craft_encode(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_craft c;
	if (!craft_from_bin(h, a[0].s, &a[1], &c, h->err, sizeof h->err)) {
		r->s = trs_strdup_printf("%s", "");
		return;
	}
	trs_json *j = trs_craft_to_json(&c);
	r->s = trs_json_write(j);
	trs_json_free(j);
	trs_craft_free(&c);
	h->err[0] = 0;
}

/* craft_normalize(json_text) -> canonical save text ("" + error via last_error) */
static void m_craft_normalize(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_craft c;
	if (!parse_craft(h, a[0].s, &c, h->err, sizeof h->err)) {
		r->s = trs_strdup_printf("%s", "");
		return;
	}
	trs_json *j = trs_craft_to_json(&c);
	r->s = trs_json_write(j);
	trs_json_free(j);
	trs_craft_free(&c);
	h->err[0] = 0;
}

static void m_last_error(void *self, const trs_val *a, trs_val *r) {
	r->s = trs_strdup_printf("%s", H(self)->err);
}

/* feed_aim(meta, floats, part_id, parent, line[7]) -> [target, x, y, z]:
 * where a feed line placed on `parent` at pos/rot line[0..6] would pipe to
 * (target -1 = nothing within reach) and the end of its reach, vessel-local.
 * The editor's aiming preview: the same march the flight sim does. */
static void m_feed_aim(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_craft c;
	char err[256];
	r->arr.n = 4;
	r->arr.data = malloc(sizeof(double) * 4);
	r->arr.data[0] = -1;
	r->arr.data[1] = r->arr.data[2] = r->arr.data[3] = 0;
	int def = h->catalog_loaded ? trs_catalog_find(&h->catalog, a[2].s) : -1;
	if (def < 0 || a[4].arr.n < 7 || !craft_from_bin(h, a[0].s, &a[1], &c, err, sizeof err))
		return;
	int parent = (int)a[3].i;
	if (parent >= 0 && parent < c.count) {
		const double *f = a[4].arr.data;
		trs_quat q = { f[3], f[4], f[5], f[6] };
		int k = trs_craft_add(&c, def, parent, "", "", v3(f[0], f[1], f[2]), q, -1, 0);
		int *group = malloc(sizeof(int) * (size_t)c.count);
		trs_craft_fuel_groups(&c, group);
		double dist = 0;
		r->arr.data[0] = trs_craft_feed_target(&c, group, k, &dist);
		trs_v3 end = v3_add(c.parts[k].pos, v3_scale(quat_rotate(q, v3(0, 0, 1)), dist));
		r->arr.data[1] = end.x, r->arr.data[2] = end.y, r->arr.data[3] = end.z;
		free(group);
	}
	trs_craft_free(&c);
}

/* craft_stats(json_text, body_id) -> JSON readouts. TWR/SL use that body's
 * surface gravity and sea-level pressure (vacuum figures need no body). */
static void stats_json(trs_sim_handle *h, trs_craft *cp, const char *body_id, trs_val *r);

static void stats_error(trs_val *r, const char *err) {
	trs_json *out = trs_json_new(TRS_JSON_OBJECT);
	trs_json_set(out, "error", trs_json_new_str(err));
	r->s = trs_json_write_compact(out);
	trs_json_free(out);
}

static void m_craft_stats(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_craft c;
	char err[256];
	if (!parse_craft(h, a[0].s, &c, err, sizeof err)) {
		stats_error(r, err);
		return;
	}
	stats_json(h, &c, a[1].s, r);
}

/* craft_stats_bin(meta, floats, body_id) -> same readouts as craft_stats */
static void m_craft_stats_bin(void *self, const trs_val *a, trs_val *r) {
	trs_sim_handle *h = H(self);
	trs_craft c;
	char err[256];
	if (!craft_from_bin(h, a[0].s, &a[1], &c, err, sizeof err)) {
		stats_error(r, err);
		return;
	}
	stats_json(h, &c, a[2].s, r);
}

static void stats_json(trs_sim_handle *h, trs_craft *cp, const char *body_id, trs_val *r) {
	trs_craft c = *cp;
	trs_json *out = trs_json_new(TRS_JSON_OBJECT);
	double g = 9.80665, p_sl = TRS_P_REF, rho_sl = 1.225;
	if (h->world) {
		int b = trs_system_find(&h->world->sys, body_id);
		if (b >= 0) {
			const trs_body *bd = &h->world->sys.bodies[b];
			g = bd->mu / (bd->radius * bd->radius);
			p_sl = bd->atm.p0;
			rho_sl = bd->atm.rho0;
		}
	}
	double mass;
	trs_v3 cm;
	trs_craft_mass_props(&c, &mass, &cm, NULL);
	double dry = 0;
	int crew = 0;
	for (int i = 0; i < c.count; i++) {
		dry += trs_craft_def(&c, i)->mass;
		crew += trs_craft_def(&c, i)->crew;
	}
	trs_stage_stats st[64];
	int ns = trs_craft_stage_stats(&c, trs_craft_max_stage(&c), g, p_sl, st, 64);
	trs_json_set(out, "name", trs_json_new_str(c.name));
	/* liquid engines with no tank they can draw from (a decoupler between
	 * them and every tank blocks the feed): they would light and do nothing */
	{
		trs_fuel_net net;
		trs_fuel_net_build(&net, &c);
		trs_json *unfed = trs_json_new(TRS_JSON_ARRAY);
		for (int i = 0; i < c.count; i++)
			if (trs_craft_def(&c, i)->propellant == TRS_PROP_LIQUID && !trs_craft_engine_fed(&c, &net, i))
				trs_json_push(unfed, trs_json_new_num(i));
		trs_json_set(out, "unfed_engines", unfed);
		/* feed lines: [line part, part its outlet reaches or -1] */
		trs_json *lines = trs_json_new(TRS_JSON_ARRAY);
		for (int k = 0; k < net.nlines; k++) {
			trs_json *pair = trs_json_new(TRS_JSON_ARRAY);
			trs_json_push(pair, trs_json_new_num(net.line_part[k]));
			trs_json_push(pair, trs_json_new_num(net.line_target[k]));
			trs_json_push(lines, pair);
		}
		trs_json_set(out, "feed_lines", lines);
		trs_fuel_net_free(&net);
	}
	/* parts an engine of the same piece fires into hard enough to burn
	 * them up: [part, engine, SPACE stage, skin temperature it settles at].
	 * Checked as each stage lights, with what that stage still holds
	 * together; the vacuum jet (the hottest). Engines no propellant can
	 * reach never light and are left out. */
	{
		trs_json *hits = trs_json_new(TRS_JSON_ARRAY);
		int n = c.count, top = trs_craft_max_stage(&c);
		unsigned char *masks = calloc((size_t)(n ? n : 1) * (size_t)(n ? n : 1), 1);
		unsigned char *done = calloc((size_t)(n ? n : 1), 1);
		trs_fuel_net net;
		trs_fuel_net_build(&net, &c);
		for (int d = 0; d < n; d++)
			if (trs_craft_def(&c, d)->decoupler && c.parts[d].stage >= 0)
				trs_craft_detach_set(&c, d, masks + (size_t)d * n);
		for (int s = top; s >= 0; s--) {
			for (int i = 0; i < n; i++) {
				if (done[i])
					continue;
				double q = 0, worst = 0;
				int worst_e = -1;
				for (int e = 0; e < n; e++) {
					const trs_part_def *de = trs_craft_def(&c, e);
					if (de->propellant == TRS_PROP_NONE || c.parts[e].stage < s || e == i)
						continue;
					if (de->propellant == TRS_PROP_LIQUID ? !trs_craft_engine_fed(&c, &net, e) : c.parts[e].fuel <= 0)
						continue; /* never burns */
					int apart = 0;
					for (int d = 0; d < n && !apart; d++)
						if (trs_craft_def(&c, d)->decoupler && c.parts[d].stage >= s)
							apart = masks[(size_t)d * n + e] != masks[(size_t)d * n + i];
					if (apart)
						continue;
					double f = trs_plume_flux(&c, e, i, trs_part_mdot(&c, e), de->isp_vac);
					q += f;
					if (f > worst)
						worst = f, worst_e = e;
				}
				double soak = q > 0 ? trs_plume_soak_temp(&c, i, q) : 0;
				if (worst_e >= 0 && soak > trs_craft_def(&c, i)->max_temp) {
					done[i] = 1;
					trs_json *hit = trs_json_new(TRS_JSON_ARRAY);
					trs_json_push(hit, trs_json_new_num(i));
					trs_json_push(hit, trs_json_new_num(worst_e));
					trs_json_push(hit, trs_json_new_num(s));
					trs_json_push(hit, trs_json_new_num(soak));
					trs_json_push(hits, hit);
				}
			}
		}
		free(masks);
		free(done);
		trs_fuel_net_free(&net);
		trs_json_set(out, "plume_hits", hits);
	}
	trs_json_set(out, "parts", trs_json_new_num(c.count));
	trs_json_set(out, "mass", trs_json_new_num(mass));
	trs_json_set(out, "dry_mass", trs_json_new_num(dry));
	/* RCS: monopropellant aboard, thruster blocks, and the delta-v a pure
	 * slide gets from all of it in vacuum (thrust-weighted Isp) */
	{
		double mono_cap, mono = trs_craft_mono(&c, &mono_cap), thrust = 0, flow = 0;
		int blocks = 0;
		for (int i = 0; i < c.count; i++) {
			const trs_part_def *d = trs_craft_def(&c, i);
			if (d->rcs_n > 0) {
				blocks++;
				thrust += d->rcs_n * d->rcs_thrust;
				flow += d->rcs_n * d->rcs_thrust / d->rcs_isp_vac;
			}
		}
		trs_json_set(out, "mono", trs_json_new_num(mono));
		trs_json_set(out, "rcs_blocks", trs_json_new_num(blocks));
		trs_json_set(out, "rcs_dv", trs_json_new_num(blocks && mono > 0 ? thrust / flow * TRS_G0 * log(mass / (mass - mono)) : 0));
	}
	/* jets: standing at this body's sea level, full throttle: thrust, TWR
	 * and how long all the liquid fuel aboard would feed them */
	{
		double jt = 0, jflow = 0, liquid = 0;
		int jets = 0;
		for (int i = 0; i < c.count; i++) {
			const trs_part_def *d = trs_craft_def(&c, i);
			if (d->fuel_liquid > 0)
				liquid += c.parts[i].fuel;
			if (d->air_breathing) {
				double f = trs_engine_air_factor(d, rho_sl, 0);
				jets++;
				jt += d->thrust_vac * f;
				jflow += trs_engine_mdot(d) * f;
			}
		}
		trs_json_set(out, "jets", trs_json_new_num(jets));
		trs_json_set(out, "jet_thrust_sl", trs_json_new_num(jt));
		trs_json_set(out, "jet_twr_sl", trs_json_new_num(jt / (mass * g)));
		trs_json_set(out, "jet_time_sl", trs_json_new_num(jflow > 0 ? liquid / jflow : 0));
	}
	/* electricity: stored, solar face-on at each panel's reference
	 * distance (Floe's), drawn idle, and drawn with the wheels flat out */
	{
		double cap, ec = trs_craft_ec(&c, &cap), solar = 0, draw = 0, wheels = 0;
		for (int i = 0; i < c.count; i++) {
			const trs_part_def *d = trs_craft_def(&c, i);
			solar += d->solar_kw;
			draw += d->ec_draw;
			wheels += d->torque_kw_per_knm * d->torque / 1000.0;
		}
		trs_json_set(out, "ec", trs_json_new_num(ec));
		trs_json_set(out, "ec_capacity", trs_json_new_num(cap));
		trs_json_set(out, "solar_kw", trs_json_new_num(solar));
		trs_json_set(out, "ec_draw", trs_json_new_num(draw));
		trs_json_set(out, "wheel_kw", trs_json_new_num(wheels));
	}
	trs_json_set(out, "crew", trs_json_new_num(crew));
	trs_json *com = trs_json_new(TRS_JSON_ARRAY);
	trs_json_push(com, trs_json_new_num(cm.x));
	trs_json_push(com, trs_json_new_num(cm.y));
	trs_json_push(com, trs_json_new_num(cm.z));
	trs_json_set(out, "com", com);
	trs_json *stages = trs_json_new(TRS_JSON_ARRAY);
	double dv_vac = 0, dv_sl = 0;
	for (int k = 0; k < ns; k++) {
		trs_json *s = trs_json_new(TRS_JSON_OBJECT);
		trs_json_set(s, "stage", trs_json_new_num(st[k].stage));
		trs_json_set(s, "mass_start", trs_json_new_num(st[k].mass_start));
		trs_json_set(s, "mass_end", trs_json_new_num(st[k].mass_end));
		trs_json_set(s, "dv_vac", trs_json_new_num(st[k].dv_vac));
		trs_json_set(s, "dv_sl", trs_json_new_num(st[k].dv_sl));
		trs_json_set(s, "twr_vac", trs_json_new_num(st[k].twr_vac));
		trs_json_set(s, "twr_sl", trs_json_new_num(st[k].twr_sl));
		trs_json_set(s, "thrust_vac", trs_json_new_num(st[k].thrust_vac));
		trs_json_set(s, "thrust_sl", trs_json_new_num(st[k].thrust_sl));
		trs_json_set(s, "burn_time", trs_json_new_num(st[k].burn_time));
		trs_json_set(s, "isp_vac", trs_json_new_num(st[k].isp_vac));
		trs_json_set(s, "isp_sl", trs_json_new_num(st[k].isp_sl));
		trs_json_push(stages, s);
		dv_vac += st[k].dv_vac;
		dv_sl += st[k].dv_sl;
	}
	trs_json_set(out, "stages", stages);
	trs_json_set(out, "dv_vac", trs_json_new_num(dv_vac));
	trs_json_set(out, "dv_sl", trs_json_new_num(dv_sl));
	r->s = trs_json_write_compact(out);
	trs_json_free(out);
	trs_craft_free(&c);
}

#define S TRS_T_STRING
#define A TRS_T_F64ARR
const trs_method_def TRS_SIM_CRAFT_METHODS[] = {
	{ "load_catalog", m_load_catalog, S, 1, { S }, { "path" } },
	{ "craft_decode", m_craft_decode, S, 1, { S }, { "text" } },
	{ "craft_decode_floats", m_craft_decode_floats, A, 1, { S }, { "text" } },
	{ "craft_encode", m_craft_encode, S, 2, { S, A }, { "meta", "floats" } },
	{ "craft_stats_bin", m_craft_stats_bin, S, 3, { S, A, S }, { "meta", "floats", "body_id" } },
	{ "craft_normalize", m_craft_normalize, S, 1, { S }, { "json" } },
	{ "craft_stats", m_craft_stats, S, 2, { S, S }, { "json", "body_id" } },
	{ "last_error", m_last_error, S, 0, { 0 }, { 0 } },
	{ "feed_aim", m_feed_aim, A, 5, { S, A, S, TRS_T_INT, A }, { "meta", "floats", "part_id", "parent", "line" } },
};
const int TRS_SIM_CRAFT_METHOD_COUNT = sizeof(TRS_SIM_CRAFT_METHODS) / sizeof(TRS_SIM_CRAFT_METHODS[0]);
