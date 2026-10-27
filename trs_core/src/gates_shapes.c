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

/* test-shapes: parts of any size and shape.
 *   1. a resized tank holds and weighs what its volume says, its nodes and
 *      radius follow, a round part stays round, sizes clamp to the
 *      part's limits, and the size survives the craft file and the world
 *      save;
 *   2. the stage readout of a stretched tank under an engine is the
 *      rocket equation for its new masses;
 *   3. a box hull's inertia is the solid box's; a wedge's hull narrows
 *      as it rises;
 *   4. a box set down on the flat by the pad comes to rest on its bottom
 *      face, square to the ground;
 *   5. a resized box hab's room grows with it;
 *   6. the Hearth Torch pushes its full 800 kN with a Hearth Core aboard,
 *      nothing with no power, and off a battery alone exactly the impulse
 *      the battery's energy pays for. */
#include "trs/gate.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static const trs_part_catalog *CAT;

static int put(trs_craft *c, const char *id, int parent, trs_v3 pos) {
	return trs_craft_add(c, trs_catalog_find(CAT, id), parent, parent < 0 ? "" : "top", parent < 0 ? "" : "bottom", pos,
			quat_identity(), -1, 0);
}

#define REL(a, b) (fabs((a) - (b)) <= 1e-12 * fmax(1.0, fabs(b)))

trs_gate_status trs_gate_shapes(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	char path[1024], err[256];
	if (!trs_gate_load_catalog(out, data_dir, &cat))
		return TRS_GATE_FAIL;
	CAT = &cat;
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load system: %s\n", err);
		return TRS_GATE_FAIL;
	}
	int floe = trs_system_find(&sys, "floe");
	int failed = 0;
	trs_world *w = malloc(sizeof(trs_world));
	const trs_part_def *ts = &cat.defs[trs_catalog_find(&cat, "tank_stretch")];

	/* 1. a stretched tank */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		put(&c, "tank_stretch", -1, v3(0, 0, 0));
		trs_craft_set_size(&c, 0, v3(1.5, 2.0, 0.7)); /* round: z follows x */
		const trs_part_def *d = trs_craft_def(&c, 0);
		double vol = 1.5 * 2.0 * 1.5;
		int ok = c.parts[0].size.z == 1.5 && REL(d->fuel_liquid, ts->fuel_liquid * vol) && REL(d->mass, ts->mass * vol) &&
			 REL(c.parts[0].fuel, d->fuel_liquid) && REL(d->radius, ts->radius * 1.5) && REL(d->height, ts->height * 2) &&
			 REL(trs_part_node(d, "top")->pos.y, ts->height) && REL(trs_part_node(d, "top")->radius, ts->nodes[0].radius * 1.5);
		trs_craft_set_size(&c, 0, v3(9, 0.01, 9));
		int clamped = REL(c.parts[0].size.x, ts->size_max.x) && REL(c.parts[0].size.y, ts->size_min.y);
		trs_craft_set_size(&c, 0, v3(1.5, 2.0, 1.5));
		/* through the craft file and the world save */
		trs_json *j = trs_craft_to_json(&c);
		trs_craft c2;
		int back = trs_craft_from_json(&c2, &cat, j, err, sizeof err);
		trs_json_free(j);
		back = back && c2.parts[0].size.x == 1.5 && c2.parts[0].size.y == 2.0 && REL(trs_craft_def(&c2, 0)->fuel_liquid, d->fuel_liquid);
		trs_world_init(w, &sys, 0);
		const trs_body *F = &sys.bodies[floe];
		int id = trs_world_add_craft(w, &c2, floe, v3(F->radius + 3e5, 0, 0), v3(0, sqrt(F->mu / (F->radius + 3e5)), 0), quat_identity());
		w->vessels[id].craft->parts[0].fuel *= 0.5;
		trs_vessel_refresh(w, id);
		trs_json *wj = trs_world_to_json(w);
		trs_world *w2 = malloc(sizeof(trs_world));
		trs_world_init(w2, &sys, 0);
		int wb = trs_world_from_json(w2, &cat, wj, err, sizeof err);
		trs_json_free(wj);
		wb = wb && w2->vessels[id].craft->parts[0].size.y == 2.0 && REL(w2->vessels[id].craft->parts[0].fuel, w->vessels[id].craft->parts[0].fuel) &&
		     REL(w2->vessels[id].mass, w->vessels[id].mass);
		ok = ok && clamped && back && wb;
		failed += !ok;
		trs_log_printf(out, "%s stretch tank at 1.5 x 2 (z asked 0.7, kept round at 1.5): %.0f kg of propellant (%.2f x), %.0f kg dry, radius %.4f m, top node %.3f m; "
				"clamps to %.1f x %.1f; craft file %s, world save %s\n",
				ok ? "ok  " : "FAIL", d->fuel_liquid, d->fuel_liquid / ts->fuel_liquid, d->mass, d->radius, trs_part_node(d, "top")->pos.y,
				ts->size_max.x, ts->size_min.y, back ? "keeps it" : "LOSES it", wb ? "keeps it" : "LOSES it");
		trs_world_free(w2);
		free(w2);
		trs_world_free(w);
		trs_craft_free(&c2);

		/* 2. the readout: an engine under it */
		int e = put(&c, "engine_big", 0, v3(0, -0.5 * d->height - 1.1, 0));
		trs_craft_activate_stage_root_only(&c, -1);
		c.parts[e].stage = 0;
		trs_stage_stats st[8];
		int n = trs_craft_stage_stats(&c, 0, 9.80665, 0, st, 8);
		const trs_part_def *ed = trs_craft_def(&c, e);
		double m0 = d->mass + d->fuel_liquid + ed->mass, m1 = d->mass + ed->mass;
		double want = ed->isp_vac * TRS_G0 * log(m0 / m1);
		ok = n == 1 && fabs(st[0].dv_vac - want) < 1e-6 * want && REL(st[0].mass_start, m0);
		failed += !ok;
		trs_log_printf(out, "%s stage readout: %.2f m/s from %.0f to %.0f kg (rocket equation %.2f m/s)\n", ok ? "ok  " : "FAIL", n ? st[0].dv_vac : 0.0,
				m0, m1, want);
		trs_craft_free(&c);
	}

	/* 3. a box's inertia, and a wedge's taper */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		put(&c, "hull_box", -1, v3(0, 0, 0));
		trs_craft_set_size(&c, 0, v3(2.0, 1.2, 0.5));
		const trs_part_def *d = trs_craft_def(&c, 0);
		double m, W = d->width_bottom, H = d->height, D = d->depth_bottom;
		trs_v3 cm;
		trs_m3 I;
		trs_craft_mass_props(&c, &m, &cm, &I);
		int ok = REL(W, 5.0) && REL(H, 3.0) && REL(D, 1.25) && REL(I.m[0][0], m * (H * H + D * D) / 12) && REL(I.m[1][1], m * (W * W + D * D) / 12) &&
			 REL(I.m[2][2], m * (W * W + H * H) / 12);
		const trs_part_def *wd = &cat.defs[trs_catalog_find(&cat, "hull_wedge")];
		trs_v3 nn[2];
		double dd[2];
		int nf;
		double hh = wd->height * 0.5, near_top = hh * 0.9;
		int in_low = trs_hull_inside(wd, v3(0, -hh * 0.9, wd->depth_bottom * 0.45), nn, dd, &nf);
		int out_high = !trs_hull_inside(wd, v3(0, near_top, wd->depth_bottom * 0.45), nn, dd, &nf);
		int in_high = trs_hull_inside(wd, v3(wd->width_top * 0.45, near_top, 0), nn, dd, &nf);
		ok = ok && in_low && out_high && in_high;
		failed += !ok;
		trs_log_printf(out, "%s box %.2f x %.2f x %.2f m, %.0f kg: inertia %.1f, %.1f, %.1f kg m2 = the solid box's; wedge: a point %.2f m out is inside low down, outside near the edge at the top (still inside across it)\n",
				ok ? "ok  " : "FAIL", W, H, D, m, I.m[0][0], I.m[1][1], I.m[2][2], wd->depth_bottom * 0.45);
		trs_craft_free(&c);
	}

	/* 4. a box set down on the flat */
	{
		trs_world_init(w, &sys, 0);
		const trs_body *F = &sys.bodies[floe];
		trs_craft c;
		trs_craft_init(&c, &cat);
		put(&c, "hull_box", -1, v3(0, 0, 0));
		trs_craft_set_size(&c, 0, v3(1.6, 0.8, 1.2));
		const trs_part_def *d = trs_craft_def(&c, 0);
		/* 60 m east of the pad (inside its flattened ground), 1 m up */
		trs_v3 up = F->terrain.site_dir, e = v3_norm(v3_cross(v3(0, 0, 1), up));
		trs_v3 dir = v3_norm(v3_add(up, v3_scale(e, 60.0 / F->radius)));
		double g = F->radius + trs_terrain_height(&F->terrain, F->radius, dir);
		trs_v3 r = v3_scale(dir, g + 0.5 * d->height + 1.0);
		trs_quat q = quat_from_to(v3(0, 1, 0), dir);
		int id = trs_world_add_craft(w, &c, floe, trs_body_from_fixed(F, 0, r), v3_cross(trs_body_omega(F), trs_body_from_fixed(F, 0, r)),
				quat_mul(trs_body_quat(F, 0), q));
		trs_vessel *vs = &w->vessels[id];
		vs->sas = TRS_SAS_OFF;
		for (int k = 0; k < 50 * 8; k++)
			trs_world_step_physics(w);
		trs_v3 rf = trs_body_to_fixed(F, w->t, vs->r);
		double above = v3_len(rf) - (F->radius + trs_terrain_height(&F->terrain, F->radius, v3_norm(rf)));
		double tilt = acos(fmin(1, v3_dot(quat_rotate(vs->q, v3(0, 1, 0)), v3_norm(vs->r))));
		/* (the hull's contact springs give a little under its weight, as any part's do) */
		int ok = vs->landed && above < 0.5 * d->height && above > 0.5 * d->height - 0.2 && tilt < 0.02;
		failed += !ok;
		trs_log_printf(out, "%s a %.1f x %.1f x %.1f m box dropped by the pad: rests %.3f m up (half its height %.3f), %.2f deg off upright, %s\n",
				ok ? "ok  " : "FAIL", d->width_bottom, d->height, d->depth_bottom, above, 0.5 * d->height, tilt * 180 / TRS_PI,
				vs->landed ? "landed" : "NOT landed");
		trs_world_free(w);
		trs_craft_free(&c);
	}

	/* 5. a bigger box hab, a bigger room */
	{
		trs_craft c;
		trs_craft_init(&c, &cat);
		put(&c, "hab_box", -1, v3(0, 0, 0));
		trs_craft_set_size(&c, 0, v3(2.0, 1.0, 1.0));
		const trs_part_def *d = trs_craft_def(&c, 0);
		double in = trs_craft_room_depth(&c, NULL, 0, v3(2.3, 0, 0)), outside = trs_craft_room_depth(&c, NULL, 0, v3(2.6, 0, 0));
		int ok = in > 0 && outside < 0 && d->crew == 8;
		failed += !ok;
		trs_log_printf(out, "%s box hab at twice the width: room reaches %.2f m out (2.3 m is %.2f m inside it), crew %d\n", ok ? "ok  " : "FAIL",
				d->volumes[0].hi.x, in, d->crew);
		trs_craft_free(&c);
	}

	/* 6. the Hearth Torch on its power */
	{
		const char *cfg[3] = { "a Hearth Core", "nothing to power it", "a battery alone" };
		double impulse[3] = { 0 };
		for (int k = 0; k < 3; k++) {
			trs_world_init(w, &sys, 0);
			trs_craft c;
			trs_craft_init(&c, &cat);
			int tank = put(&c, "tank_stretch", -1, v3(0, 0, 0));
			const trs_part_def *td = trs_craft_def(&c, tank);
			int eng = put(&c, "hearth_torch", tank, v3(0, -0.5 * td->height - 1.0, 0));
			if (k == 0)
				put(&c, "hearth_core", tank, v3(0, 0.5 * td->height + 1.0, 0));
			if (k == 2) {
				int b = trs_craft_add(&c, trs_catalog_find(&cat, "battery"), tank, "", "", v3(1.3, 0, 0), quat_identity(), -1, 0);
				(void)b;
			}
			c.parts[eng].engine_on = 1;
			const trs_body *F = &sys.bodies[floe];
			double rr = F->radius + 5e5;
			int id = trs_world_add_craft(w, &c, floe, v3(rr, 0, 0), v3(0, sqrt(F->mu / rr), 0), quat_identity());
			trs_vessel *vs = &w->vessels[id];
			vs->sas = TRS_SAS_OFF;
			vs->throttle = 1;
			double ec0 = trs_craft_ec(vs->craft, NULL);
			double thrust = 0;
			for (int s = 0; s < 50; s++) {
				double m0 = vs->mass; /* what the push was on */
				trs_world_step_physics(w);
				if (s == 0)
					thrust = v3_len(vs->sensed_accel) * m0;
				impulse[k] += v3_len(vs->sensed_accel) * m0 * TRS_PHYSICS_DT;
			}
			const trs_part_def *ed = trs_craft_def(vs->craft, eng);
			int ok;
			if (k == 0)
				ok = fabs(thrust - ed->thrust_vac) < 1e-6 * ed->thrust_vac;
			else if (k == 1)
				ok = impulse[1] == 0;
			else {
				double want = ed->thrust_vac * ec0 / ed->engine_kw;
				ok = fabs(impulse[2] - want) < 1e-3 * want && trs_craft_ec(vs->craft, NULL) < 1e-9;
				trs_log_printf(out, "     (battery %.0f kJ pays for %.4f s at %.0f kW: %.0f N s due)\n", ec0, ec0 / ed->engine_kw, ed->engine_kw, want);
			}
			failed += !ok;
			trs_log_printf(out, "%s Hearth Torch with %s: first tick %.0f kN, 1 s impulse %.0f N s\n", ok ? "ok  " : "FAIL", cfg[k], thrust / 1000, impulse[k]);
			trs_world_free(w);
			trs_craft_free(&c);
		}
	}

	free(w);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
