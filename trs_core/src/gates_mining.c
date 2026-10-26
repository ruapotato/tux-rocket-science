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

/* test-mining: a drill within reach of the ground digs drill rate x the
 * body's ore abundance for its power; one out of reach (or in orbit)
 * digs nothing; a refinery turns ore into 0.8 x as much propellant; a full
 * bin, a full tank or a flat battery stops them; and a landed craft left
 * mining through time warp ends up where the same hours in normal time
 * put it. */
#include "trs/gate.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static void run(trs_world *w, double s) {
	for (int k = 0; k < (int)lround(s * TRS_PHYSICS_HZ); k++)
		trs_world_step_physics(w);
}

/* tank_m / ore_bin / refinery / probe_core, two batteries, and a drill on the
 * tank's flank `drill_up` m above its bottom */
static void build(trs_craft *c, const trs_part_catalog *cat, double drill_up, double *half) {
	trs_craft_init(c, cat);
	int ti = trs_catalog_find(cat, "tank_m"), bi = trs_catalog_find(cat, "ore_bin"), ri = trs_catalog_find(cat, "refinery"),
	    pi = trs_catalog_find(cat, "probe_core"), di = trs_catalog_find(cat, "drill"), ei = trs_catalog_find(cat, "battery");
	const trs_part_def *td = &cat->defs[ti], *bd = &cat->defs[bi], *rd = &cat->defs[ri], *pd = &cat->defs[pi];
	double y = 0;
	trs_craft_add(c, ti, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	y = td->height * 0.5 + bd->height * 0.5;
	trs_craft_add(c, bi, 0, "bottom", "top", v3(0, y, 0), quat_identity(), -1, 0);
	y += bd->height * 0.5 + rd->height * 0.5;
	trs_craft_add(c, ri, 1, "bottom", "top", v3(0, y, 0), quat_identity(), -1, 0);
	y += rd->height * 0.5 + pd->height * 0.5;
	trs_craft_add(c, pi, 2, "bottom", "top", v3(0, y, 0), quat_identity(), -1, 0);
	trs_craft_add(c, ei, 0, "", "", v3(td->radius, 0, 0), quat_from_to(v3(0, 0, 1), v3(1, 0, 0)), -1, 0);
	trs_craft_add(c, ei, 0, "", "", v3(-td->radius, 0, 0), quat_from_to(v3(0, 0, 1), v3(-1, 0, 0)), -1, 0);
	trs_craft_add(c, di, 0, "", "", v3(0, -td->height * 0.5 + drill_up, td->radius), quat_identity(), -1, 0);
	*half = td->height * 0.5;
}

/* standing upright on body bi under +X, settled */
static int stand(trs_world *w, const trs_craft *c, int bi, double half) {
	const trs_body *b = &w->sys.bodies[bi];
	double ground = trs_body_terrain_at(b, w->t, v3(b->radius, 0, 0));
	double cm_y;
	{
		trs_v3 cm;
		double m;
		trs_craft_mass_props(c, &m, &cm, NULL);
		cm_y = cm.y; /* the state is the centre of mass's: the tank's bottom goes 5 cm up */
	}
	trs_v3 r = v3(b->radius + ground + half + cm_y + 0.05, 0, 0);
	int id = trs_world_add_craft(w, c, bi, r, v3_cross(trs_body_omega(b), r), quat_from_to(v3(0, 1, 0), v3(1, 0, 0)));
	w->vessels[id].w = trs_body_omega(b);
	w->vessels[id].sas = TRS_SAS_STABILITY;
	run(w, 3);
	w->vessels[id].sas = TRS_SAS_OFF; /* no wheel draw muddling the power sums */
	return id;
}

static double liquid(const trs_craft *c) {
	double f = 0;
	for (int i = 0; i < c->count; i++)
		if (trs_craft_def(c, i)->fuel_liquid > 0)
			f += c->parts[i].fuel;
	return f;
}

static void only(trs_craft *c, int drills, int refineries) {
	for (int i = 0; i < c->count; i++) {
		const trs_part_def *d = trs_craft_def(c, i);
		if (d->drill_kg_s > 0)
			c->parts[i].running = drills;
		if (d->refine_kg_s > 0)
			c->parts[i].running = refineries;
	}
}

#define NEAR(a, b, tol) (fabs((a) - (b)) <= (tol))

trs_gate_status trs_gate_mining(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	char path[1024], err[256];
	if (!trs_gate_load_catalog(out, data_dir, &cat))
		return TRS_GATE_FAIL;
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int krill = trs_system_find(&sys, "krill");
	const trs_body *K = &sys.bodies[krill];
	const trs_part_def *dd = &cat.defs[trs_catalog_find(&cat, "drill")], *rd = &cat.defs[trs_catalog_find(&cat, "refinery")],
			   *pd = &cat.defs[trs_catalog_find(&cat, "probe_core")];
	int failed = 0;
	double half;
	trs_craft c;
	trs_world *w = malloc(sizeof(trs_world));

	/* 1. drill low on the flank, landed on Krill: 10 s of digging */
	build(&c, &cat, 0.3, &half);
	trs_world_init(w, &sys, 0);
	int id = stand(w, &c, krill, half);
	trs_vessel *vs = &w->vessels[id];
	only(vs->craft, 1, 0);
	double ore0 = trs_craft_ore(vs->craft, NULL), ec0 = trs_craft_ec(vs->craft, NULL), m0 = vs->mass;
	run(w, 10);
	double dug = trs_craft_ore(vs->craft, NULL) - ore0, used = ec0 - trs_craft_ec(vs->craft, NULL);
	double want = dd->drill_kg_s * K->ore * 10, want_kj = (dd->drill_kw + pd->ec_draw) * 10;
	int ok = vs->landed && vs->drill_ground == 1 && NEAR(dug, want, 1e-9) && NEAR(used, want_kj, 1e-6) && NEAR(vs->mass - m0, dug, 1e-9);
	failed += !ok;
	trs_log_printf(out, "%s drill 0.3 m up the flank, landed on Krill (ore %.2f): %.4f kg in 10 s (want %.2f x %.2f x 10 = %.4f), %.3f kJ (drill %.2f + core %.2f kW), mass +%.4f kg\n",
			ok ? "ok  " : "FAIL", K->ore, dug, dd->drill_kg_s, K->ore, want, used, dd->drill_kw, pd->ec_draw, vs->mass - m0);

	/* 2. the refinery: 10 s turns 5 kg of ore into 4 kg of propellant */
	only(vs->craft, 0, 1);
	trs_craft_ore_add(vs->craft, 200);
	for (int i = 0; i < vs->craft->count; i++)
		if (trs_craft_def(vs->craft, i)->fuel_liquid > 0)
			vs->craft->parts[i].fuel *= 0.5;
	ore0 = trs_craft_ore(vs->craft, NULL), ec0 = trs_craft_ec(vs->craft, NULL);
	double f0 = liquid(vs->craft);
	run(w, 10);
	double ore_used = ore0 - trs_craft_ore(vs->craft, NULL), made = liquid(vs->craft) - f0;
	used = ec0 - trs_craft_ec(vs->craft, NULL);
	ok = NEAR(ore_used, rd->refine_kg_s * 10, 1e-9) && NEAR(made, rd->refine_kg_s * rd->refine_yield * 10, 1e-9) &&
	     NEAR(used, (rd->refine_kw + pd->ec_draw) * 10, 1e-6);
	failed += !ok;
	trs_log_printf(out, "%s refinery 10 s: ore -%.4f kg, propellant +%.4f kg (want %.2f and %.2f), %.3f kJ\n", ok ? "ok  " : "FAIL",
			ore_used, made, rd->refine_kg_s * 10, rd->refine_kg_s * rd->refine_yield * 10, used);

	/* 3. a full bin stops the drill (and its draw); a full tank the refinery */
	only(vs->craft, 1, 0);
	double cap;
	trs_craft_ore(vs->craft, &cap);
	trs_craft_ore_add(vs->craft, cap);
	ec0 = trs_craft_ec(vs->craft, NULL);
	run(w, 5);
	double full_dug = trs_craft_ore(vs->craft, NULL) - cap;
	double full_kj = ec0 - trs_craft_ec(vs->craft, NULL);
	only(vs->craft, 0, 1);
	for (int i = 0; i < vs->craft->count; i++)
		if (trs_craft_def(vs->craft, i)->fuel_liquid > 0)
			vs->craft->parts[i].fuel = trs_craft_def(vs->craft, i)->fuel_liquid;
	ore0 = trs_craft_ore(vs->craft, NULL);
	run(w, 5);
	double full_ref = ore0 - trs_craft_ore(vs->craft, NULL);
	ok = full_dug == 0 && NEAR(full_kj, pd->ec_draw * 5, 1e-6) && full_ref == 0;
	failed += !ok;
	trs_log_printf(out, "%s bin full: drill digs %.4f kg and draws only the core's %.3f kJ in 5 s; tanks full: refinery uses %.4f kg\n",
			ok ? "ok  " : "FAIL", full_dug, full_kj, full_ref);

	/* 4. a flat battery: nothing turns */
	only(vs->craft, 1, 1);
	trs_craft_ore_add(vs->craft, -cap);
	trs_craft_ec_add(vs->craft, -1e9);
	run(w, 5);
	double flat = trs_craft_ore(vs->craft, NULL);
	ok = flat == 0 && vs->ore_rate == 0;
	failed += !ok;
	trs_log_printf(out, "%s battery flat: %.4f kg dug in 5 s\n", ok ? "ok  " : "FAIL", flat);
	trs_world_free(w);
	trs_craft_free(&c);

	/* 5. drill mounted 3 m up: out of reach of the ground */
	build(&c, &cat, 2.6, &half);
	trs_world_init(w, &sys, 0);
	id = stand(w, &c, krill, half);
	vs = &w->vessels[id];
	only(vs->craft, 1, 0);
	run(w, 5);
	double high = trs_craft_ore(vs->craft, NULL);
	ok = vs->landed && vs->drill_ground == 0 && high == 0;
	failed += !ok;
	trs_log_printf(out, "%s drill 2.6 m up the side (reach %.1f m): %.4f kg in 5 s\n", ok ? "ok  " : "FAIL",
			dd->drill_reach, high);
	trs_world_free(w);
	trs_craft_free(&c);

	/* 6. an hour of mining, drill and refinery both on, tank half empty:
	 *    normal time against LANDED time warp (on the batteries alone, so
	 *    both stop when the charge does) */
	double res[2][3];
	for (int warp = 0; warp < 2; warp++) {
		build(&c, &cat, 0.3, &half);
		trs_world_init(w, &sys, 0);
		id = stand(w, &c, krill, half);
		vs = &w->vessels[id];
		for (int i = 0; i < vs->craft->count; i++)
			if (trs_craft_def(vs->craft, i)->fuel_liquid > 0)
				vs->craft->parts[i].fuel *= 0.5;
		only(vs->craft, 1, 1);
		ec0 = trs_craft_ec(vs->craft, NULL);
		f0 = liquid(vs->craft);
		if (warp) {
			trs_vessel_set_mode(w, id, TRS_MODE_LANDED);
			trs_event ev;
			trs_world_warp(w, 3600, -1, &ev);
		} else
			run(w, 3600);
		res[warp][0] = trs_craft_ore(vs->craft, NULL);
		res[warp][1] = liquid(vs->craft) - f0;
		res[warp][2] = trs_craft_ec(vs->craft, NULL);
		trs_world_free(w);
		trs_craft_free(&c);
	}
	/* all the ore dug, refined or not; warp refines in 20 s steps, so up to
	 * one step's digging can still sit in the bin */
	double dug_n = res[0][0] + res[0][1] / rd->refine_yield, dug_w = res[1][0] + res[1][1] / rd->refine_yield;
	double step_ore = dd->drill_kg_s * K->ore * 20.0;
	ok = res[0][1] > 0 && NEAR(dug_w, dug_n, 0.01 * dug_n) && res[1][0] <= step_ore + 1e-9 && res[0][2] < 1 && res[1][2] < 1;
	failed += !ok;
	trs_log_printf(out, "%s an hour on the batteries (%.0f kJ, flat after): normal time %.2f kg ore left, +%.2f kg propellant (%.2f kg dug); "
			"time warp %.2f kg left, +%.2f kg (%.2f kg dug, %+.2f%%)\n",
			ok ? "ok  " : "FAIL", ec0, res[0][0], res[0][1], dug_n, res[1][0], res[1][1], dug_w, 100.0 * (dug_w - dug_n) / dug_n);
	free(w);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
