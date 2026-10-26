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

/* test-power: electricity. A solar panel facing the sun gives its rated
 * power x (reference distance / distance)^2, cos(60 deg) of it turned 60
 * degrees away, and nothing in Floe's shadow; a probe's battery drains at
 * its core's draw plus the reaction wheel's share of its full effort;
 * flat and in the dark, the probe answers nothing; back in the sun it
 * charges at what the panel gives less what it uses. */
#include "trs/gate.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

/* sun direction (SOI frame) and distance from it, for a vessel */
static trs_v3 sun_dir(const trs_world *w, const trs_vessel *vs, double *d) {
	trs_v3 br, bv;
	trs_body_state_abs(&w->sys, vs->body, w->t, &br, &bv);
	trs_v3 p = v3_add(br, vs->r);
	*d = v3_len(p);
	return v3_scale(p, -1.0 / *d);
}

/* the probe in a circular orbit at alt around body bi, on the sunward
 * (or night) side, its +Z (the panel's face) turned `off` rad from the sun */
static int place(trs_world *w, const trs_craft *c, int bi, double alt, int night, double off) {
	const trs_body *b = &w->sys.bodies[bi];
	trs_v3 br, bv;
	trs_body_state_abs(&w->sys, bi, w->t, &br, &bv);
	trs_v3 to_sun = v3_scale(br, -1.0 / v3_len(br));
	trs_v3 r = v3_scale(to_sun, (night ? -1 : 1) * (b->radius + alt));
	trs_v3 side = v3_norm(v3_cross(v3(0, 0, 1), r));
	trs_v3 v = v3_scale(side, sqrt(b->mu / v3_len(r)));
	int id = trs_world_add_craft(w, c, bi, r, v, quat_identity());
	trs_vessel *vs = &w->vessels[id];
	double d;
	trs_v3 s = sun_dir(w, vs, &d);
	/* +Z to the sun, then tipped `off` about an axis square to it */
	trs_quat q = quat_from_to(v3(0, 0, 1), s);
	trs_v3 tip = v3_norm(v3_cross(s, v3(0, 0, 1)));
	if (v3_len(tip) < 1e-6)
		tip = v3(1, 0, 0);
	vs->q = quat_normalize(quat_mul(quat_axis_angle(tip, off), q));
	vs->sas = TRS_SAS_OFF;
	return id;
}

trs_gate_status trs_gate_power(trs_log *out, const char *data_dir) {
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
	int floe = trs_system_find(&sys, "floe"), kelp = trs_system_find(&sys, "kelp");
	int core_i = trs_catalog_find(&cat, "probe_core"), sp_i = trs_catalog_find(&cat, "solar_panel");
	const trs_part_def *pc = &cat.defs[core_i], *sp = &cat.defs[sp_i];
	trs_craft c;
	trs_craft_init(&c, &cat);
	trs_craft_add(&c, core_i, -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	trs_craft_add(&c, sp_i, 0, "", "", v3(0, 0, pc->radius), quat_identity(), -1, 0);
	int failed = 0;

	/* 1. the panel's output */
	struct { const char *what; int body, night; double off; } cases[] = {
		{ "facing the sun over Floe", 0, 0, 0 }, { "turned 60 deg away", 0, 0, 60 }, { "in Floe's shadow", 0, 1, 0 },
		{ "facing the sun over Kelp", 1, 0, 0 },
	};
	for (int k = 0; k < 4; k++) {
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = place(w, &c, cases[k].body ? kelp : floe, 150000, cases[k].night, cases[k].off * TRS_PI / 180);
		trs_vessel *vs = &w->vessels[id];
		int lit;
		double got = trs_vessel_solar(w, vs, &lit), d;
		sun_dir(w, vs, &d);
		double want = cases[k].night ? 0 : sp->solar_kw * cos(cases[k].off * TRS_PI / 180) * pow(sp->solar_ref_m / d, 2);
		int ok = fabs(got - want) < 1e-9 && lit == !cases[k].night;
		failed += !ok;
		trs_log_printf(out, "%s panel %s: %.4f kW (want %.4f), %.3f x 10^10 m from Austra, %s\n", ok ? "ok  " : "FAIL", cases[k].what, got, want,
				d / 1e10, lit ? "sunlit" : "in shadow");
		trs_world_free(w);
		free(w);
	}
	/* 2. draining on a full pitch command in the dark, then flat */
	{
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = place(w, &c, floe, 150000, 1, 0);
		trs_vessel *vs = &w->vessels[id];
		vs->input = v3(1, 0, 0);
		double cap, e0 = trs_craft_ec(vs->craft, &cap);
		for (int s = 0; s < TRS_PHYSICS_HZ; s++)
			trs_world_step_physics(w);
		double used = e0 - trs_craft_ec(vs->craft, NULL);
		double wheel_kw = pc->torque_kw_per_knm * pc->torque / 1000.0;
		double want = pc->ec_draw + wheel_kw / 3; /* one axis of three at full effort */
		int ok = fabs(used - want) < 1e-9 && !vs->sunlit;
		failed += !ok;
		trs_log_printf(out, "%s full pitch for 1 s in the dark: %.6f kJ used (core %.3f kW + wheel %.3f kW / 3 = %.6f), battery %.1f of %.0f kJ\n",
				ok ? "ok  " : "FAIL", used, pc->ec_draw, wheel_kw, want, e0 - used, cap);
		/* flat: nothing answers */
		trs_craft_ec_add(vs->craft, -1e12);
		vs->w = v3(0, 0, 0); /* stopped dead, battery empty */
		trs_world_step_physics(w);
		trs_v3 w0 = quat_rotate(quat_conj(vs->q), vs->w);
		for (int s = 0; s < TRS_PHYSICS_HZ; s++)
			trs_world_step_physics(w);
		trs_v3 w1 = quat_rotate(quat_conj(vs->q), vs->w);
		ok = !vs->powered && fabs(w1.x - w0.x) < 1e-9 && trs_craft_ec(vs->craft, NULL) == 0;
		failed += !ok;
		trs_log_printf(out, "%s battery flat in the dark: the probe ignores full pitch (spin %.2e -> %.2e rad/s), powered %d\n", ok ? "ok  " : "FAIL",
				w0.x, w1.x, vs->powered);
		trs_world_free(w);
		free(w);
	}
	/* 3. charging in the sun from flat */
	{
		trs_world *w = malloc(sizeof(trs_world));
		trs_world_init(w, &sys, 0);
		int id = place(w, &c, floe, 150000, 0, 0);
		trs_vessel *vs = &w->vessels[id];
		trs_craft_ec_add(vs->craft, -1e12);
		int lit;
		double gen = trs_vessel_solar(w, vs, &lit);
		for (int s = 0; s < TRS_PHYSICS_HZ * 10; s++)
			trs_world_step_physics(w);
		double got = trs_craft_ec(vs->craft, NULL);
		/* the panel swings a hair off the sun over 10 s of orbit: a margin */
		double want = (gen - pc->ec_draw) * 10;
		int ok = fabs(got / want - 1) < 1e-3 && vs->powered;
		failed += !ok;
		trs_log_printf(out, "%s charging from flat in the sun: %.3f kJ in 10 s ((%.4f - %.3f) kW x 10 s = %.3f)\n", ok ? "ok  " : "FAIL", got, gen,
				pc->ec_draw, want);
		trs_world_free(w);
		free(w);
	}
	trs_craft_free(&c);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
