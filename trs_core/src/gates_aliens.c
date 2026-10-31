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

/* test-aliens: the Jaegers. A nest keeps its raiders down until a ship
 * of ours comes near, then puts them all up; on their beat they hang at
 * the nest's height and circle it. The lift drive gives what the pilot
 * asks up to its force over the ship's mass. A ship of ours that comes
 * within reach they go for: they close to a few hundred metres and their
 * lances take it apart, every hp of it a beam's tick on it. Turrets on
 * guard bring them down (wreckage, falling), the nest counts its losses
 * and makes them up after its respawn time; they go home when we leave,
 * and before a rails warp; a saved world has the same losses. */
#include "trs/gate.h"
#include "trs/json.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static void run(trs_world *w, double s) {
	for (int k = 0; k < (int)lround(s * TRS_PHYSICS_HZ); k++)
		trs_world_step_physics(w);
}

static trs_v3 ground_at(const trs_body *b, trs_v3 d) {
	d = v3_norm(d);
	return v3_scale(d, b->radius + trs_terrain_height(&b->terrain, b->radius, d));
}

/* a point m metres from body-fixed p along the ground, toward angle a (from east), on the ground */
static trs_v3 along(const trs_body *b, trs_v3 p, double a, double m) {
	trs_v3 up, n, e;
	trs_local_frame(p, &up, &n, &e);
	return ground_at(b, v3_add(p, v3_scale(v3_add(v3_scale(e, cos(a)), v3_scale(n, sin(a))), m)));
}

static int put(trs_world *w, const trs_craft *c, int bi, trs_v3 g, double up) {
	const trs_body *b = &w->sys.bodies[bi];
	trs_v3 r = trs_body_from_fixed(b, w->t, v3_madd(g, v3_norm(g), up));
	int id = trs_world_add_craft(w, c, bi, r, v3_cross(trs_body_omega(b), r), quat_from_to(v3(0, 1, 0), v3_norm(r)));
	w->vessels[id].w = trs_body_omega(b);
	trs_vessel_set_mode(w, id, TRS_MODE_LANDED);
	return id;
}

static int raiders(const trs_world *w, int code) {
	int n = 0;
	for (int i = 0; i < w->nvessels; i++)
		n += w->vessels[i].alive && w->vessels[i].pilot == code;
	return n;
}

static double ship_damage(const trs_world *w, int id) {
	double d = 0;
	for (int i = 0; i < w->vessels[id].craft->count; i++)
		d += w->vessels[id].craft->parts[i].damage;
	return d;
}

trs_gate_status trs_gate_aliens(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	char path[1024], err[256];
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_gate_load_catalog(out, data_dir, &cat) || !trs_system_load(&sys, path, err, sizeof err)) {
		trs_log_printf(out, "cannot load: %s\n", err);
		return TRS_GATE_FAIL;
	}
	trs_craft raider, probe, guard;
	trs_craft_init(&raider, &cat);
	snprintf(path, sizeof path, "%s/craft/alien/jaeger_raider.craft.json", data_dir);
	trs_json *rj = trs_json_load_file(path, err, sizeof err);
	if (!rj || !trs_craft_from_json(&raider, &cat, rj, err, sizeof err)) {
		trs_log_printf(out, "cannot load the raider: %s\n", err);
		return TRS_GATE_FAIL;
	}
	trs_json_free(rj);
	trs_craft_init(&probe, &cat);
	trs_craft_add(&probe, trs_catalog_find(&cat, "probe_core"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	trs_craft_add(&probe, trs_catalog_find(&cat, "battery"), 0, "", "", v3(0.6, 0, 0), quat_identity(), -1, 0);
	trs_craft_copy(&guard, &probe);
	int t1 = trs_craft_add(&guard, trs_catalog_find(&cat, "laser_turret"), 0, "", "", v3(0, 0.25, 0.2),
			quat_from_to(v3(0, 0, 1), v3(0, 1, 0)), -1, 0);
	int t2 = trs_craft_add(&guard, trs_catalog_find(&cat, "laser_turret"), 0, "", "", v3(0, 0.25, -0.2),
			quat_from_to(v3(0, 0, 1), v3(0, 1, 0)), -1, 0);
	(void)t1, (void)t2;
	int failed = 0;
	int gi = trs_system_find(&sys, "gloam");
	const trs_body *b = &sys.bodies[gi];
	const trs_nest *ne = &b->nests[0];
	int code = gi * TRS_MAX_NESTS + 0 + 1;
	trs_v3 home = trs_nest_home(b, 0), nest_ground = ground_at(b, ne->dir);
	double ld = 0;
	for (int i = 0; i < raider.count; i++)
		if (trs_craft_def(&raider, i)->laser_range > 0)
			ld = trs_craft_def(&raider, i)->laser_dps;
	const double t0 = 1000.0, h = TRS_PHYSICS_DT;

	/* 0. what the story says: every nest twelve kilometres north of its
	 * world's relic (a ship at the relic sees them up, but is out of their
	 * reach), its craft one we have */
	for (int bj = 0; bj < sys.count; bj++) {
		const trs_body *bb = &sys.bodies[bj];
		for (int k = 0; k < bb->nnests; k++) {
			double best = 1e300;
			for (int q = 0; q < bb->nrelics; q++)
				best = fmin(best, acos(fmin(1, v3_dot(bb->nests[k].dir, bb->relics[q].dir))) * bb->radius);
			int north = bb->nrelics > 0 && bb->nests[k].dir.z > bb->relics[0].dir.z;
			snprintf(path, sizeof path, "%s/craft/alien/%s.craft.json", data_dir, bb->nests[k].craft);
			trs_json *cj = trs_json_load_file(path, err, sizeof err);
			int have = cj != NULL;
			trs_json_free(cj);
			int okn = fabs(best - 12000) < 100 && north && have && best > bb->nests[k].detect;
			failed += !okn;
			trs_log_printf(out, "%s %s, %s: %.2f km north of its relic (the story: twelve), outside its %.0f km reach; craft %s %s\n",
					okn ? "ok  " : "FAIL", bb->name, bb->nests[k].name, best / 1000, bb->nests[k].detect / 1000, bb->nests[k].craft,
					have ? "found" : "MISSING");
		}
	}

	trs_world *w = malloc(sizeof(trs_world));
	trs_world_init(w, &sys, t0);
	trs_world_set_nest_craft(w, ne->craft, &raider);

	/* 1. down until we come near, then all of them up */
	int far = put(w, &probe, gi, along(b, nest_ground, 1.0, 40000), 1.0);
	trs_world_set_focus(w, far);
	run(w, 3.0);
	int before = raiders(w, code);
	int near = put(w, &probe, gi, along(b, nest_ground, 1.0, 10000), 1.0);
	trs_world_set_focus(w, near);
	run(w, 2.0);
	int up = raiders(w, code);
	int ok = before == 0 && up == ne->count;
	failed += !ok;
	trs_log_printf(out, "%s %s: no raiders up with us 40 km off (%d); %d up once we're 10 km off (it keeps %d)\n", ok ? "ok  " : "FAIL",
			ne->name, before, up, ne->count);

	/* 2. on their beat: at the nest's height, round it */
	run(w, 90.0);
	double worst_h = 0, worst_r = 0;
	for (int i = 0; i < w->nvessels; i++) {
		const trs_vessel *vs = &w->vessels[i];
		if (!vs->alive || vs->pilot != code)
			continue;
		trs_v3 p = trs_body_to_fixed(b, w->t, vs->r);
		trs_v3 hp = v3_scale(v3_norm(p), v3_len(home));
		worst_h = fmax(worst_h, fabs(v3_len(p) - v3_len(home)));
		worst_r = fmax(worst_r, fabs(v3_len(v3_sub(hp, home)) - 0.6 * ne->patrol));
	}
	ok = worst_h < 40 && worst_r < 300;
	failed += !ok;
	trs_log_printf(out, "%s on their beat after 90 s: within %.1f m of the nest's height, %.0f m of their %.0f m circle round it\n",
			ok ? "ok  " : "FAIL", worst_h, worst_r, 0.6 * ne->patrol);

	/* 3. the lift drive: what is asked, up to its force over the ship's mass */
	{
		int id = -1;
		for (int i = 0; i < w->nvessels; i++)
			if (w->vessels[i].alive && w->vessels[i].pilot == code)
				id = i;
		trs_vessel *vs = &w->vessels[id];
		double f = 0;
		for (int i = 0; i < vs->craft->count; i++)
			f += trs_craft_def(vs->craft, i)->lift_force;
		/* (the pilot asks; read what it got) */
		trs_world_step_physics(w);
		double asked = v3_len(vs->lift), got = v3_len(vs->lift_accel_now), most = f / vs->mass;
		ok = fabs(got - fmin(asked, most)) < 1e-9 * most;
		failed += !ok;
		trs_log_printf(out, "%s a raider's lift drive: asked %.2f m/s2, gave %.2f (its most: %.0f kN / %.0f kg = %.2f)\n", ok ? "ok  " : "FAIL",
				asked, got, f / 1000, vs->mass, most);
	}

	/* 4. an unarmed probe in reach: they come, and their lances take it apart */
	{
		int prey = put(w, &probe, gi, along(b, nest_ground, 2.5, 2000), 1.0);
		int ps = w->vessels[prey].serial;
		trs_world_set_focus(w, prey);
		trs_world_remove_vessel(w, near);
		double closest = 1e300, took = 0, beams = 0;
		int intact = 1, t_close = -1;
		for (int s = 0; s < 150 * TRS_PHYSICS_HZ && w->vessels[prey].alive && w->vessels[prey].serial == ps; s++) {
			double was = ship_damage(w, prey);
			int count = w->vessels[prey].craft->count;
			trs_world_step_physics(w);
			if (!w->vessels[prey].alive || w->vessels[prey].serial != ps)
				break;
			if (w->vessels[prey].craft->count != count)
				intact = 0;
			for (int i = 0; i < w->nvessels; i++) {
				const trs_vessel *vs = &w->vessels[i];
				if (!vs->alive || vs->pilot != code)
					continue;
				trs_v3 at;
				double rad;
				trs_thing_where(w, gi, (trs_thing){ TRS_THING_VESSEL, prey, ps }, &at, &rad);
				double d = v3_len(v3_sub(at, vs->r));
				if (d < closest)
					closest = d;
				if (d < 700 && t_close < 0)
					t_close = s;
				for (int k = 0; intact && k < vs->craft->count; k++)
					beams += vs->craft->parts[k].beam > 0 && vs->craft->parts[k].beam_hit == TRS_THING_VESSEL;
			}
			if (intact)
				took += ship_damage(w, prey) - was;
		}
		int lost = !w->vessels[prey].alive || w->vessels[prey].serial != ps || !intact;
		ok = closest < 700 && lost && fabs(took - beams * ld * h) < 1e-6;
		failed += !ok;
		trs_log_printf(out, "%s an unarmed probe 2 km from the nest: they close to %.0f m (within 700 m by %.0f s) and take it "
				"apart; it took %.1f hp before the first piece went, %.0f lance ticks on it x %.0f hp/s (%.1f)\n", ok ? "ok  " : "FAIL",
				closest, t_close * h, took, beams, ld, beams * ld * h);
		for (int i = 0; i < w->nvessels; i++)
			if (w->vessels[i].alive && !w->vessels[i].pilot && !w->vessels[i].is_alien)
				trs_world_remove_vessel(w, i);
	}

	/* 5. two turrets on guard bring them down */
	int gd = put(w, &guard, gi, along(b, nest_ground, 2.5, 2000), 1.0);
	int gs = w->vessels[gd].serial;
	trs_world_set_focus(w, gd);
	trs_world_set_guard(w, gd, 1);
	int downed = 0;
	double t_all = -1;
	for (int s = 0; s < 240 * TRS_PHYSICS_HZ && t_all < 0; s++) {
		trs_world_step_physics(w);
		for (int e = 0; e < w->nevents; e++)
			downed += w->events[e].kind == TRS_WEV_DOWNED;
		w->nevents = 0;
		if (raiders(w, code) == 0)
			t_all = s * h;
	}
	int alive = w->vessels[gd].alive && w->vessels[gd].serial == gs;
	ok = downed == ne->count && w->nest_lost[gi][0] == ne->count && t_all > 0 && alive;
	failed += !ok;
	trs_log_printf(out, "%s two turrets on guard: all %d raiders down (%d downed) in %.0f s; the nest has lost %d; our ship %s (%.0f hp taken)\n",
			ok ? "ok  " : "FAIL", ne->count, downed, t_all, w->nest_lost[gi][0], alive ? "stands" : "is lost",
			alive ? ship_damage(w, gd) : 0);
	/* the wrecks fall */
	{
		run(w, 3.0);
		int wrecks = 0, falling = 0, aloft = 0;
		for (int i = 0; i < w->nvessels; i++) {
			const trs_vessel *vs = &w->vessels[i];
			if (!vs->alive || !vs->is_alien || vs->pilot)
				continue;
			wrecks++;
			falling += v3_dot(v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r)), v3_norm(vs->r)) < -5;
		}
		run(w, 30.0);
		for (int i = 0; i < w->nvessels; i++) {
			const trs_vessel *vs = &w->vessels[i];
			if (!vs->alive || !vs->is_alien || vs->pilot)
				continue;
			trs_v3 p = trs_body_to_fixed(b, w->t, vs->r);
			aloft += v3_len(p) - b->radius - trs_terrain_height(&b->terrain, b->radius, v3_norm(p)) > 30;
		}
		ok = wrecks > 0 && falling == wrecks && aloft == 0;
		failed += !ok;
		trs_log_printf(out, "%s what is left of them falls: %d pieces, %d falling 3 s on; 30 s later none in the air (%d)\n",
				ok ? "ok  " : "FAIL", wrecks, falling, aloft);
	}

	/* 6. a saved world has the same losses */
	{
		trs_json *j = trs_world_to_json(w);
		trs_world *w2 = malloc(sizeof(trs_world));
		trs_world_init(w2, &sys, 0);
		int loaded = trs_world_from_json(w2, &cat, j, err, sizeof err);
		trs_json_free(j);
		ok = loaded && w2->nest_lost[gi][0] == w->nest_lost[gi][0] && w2->nest_lost_t[gi][0] == w->nest_lost_t[gi][0];
		failed += !ok;
		trs_log_printf(out, "%s saved and loaded: the nest's %d losses and when are the same\n", ok ? "ok  " : "FAIL", w2->nest_lost[gi][0]);
		trs_world_free(w2);
		free(w2);
	}

	/* 7. the nest makes up its losses */
	{
		double first = w->nest_lost_t[gi][0];
		w->t = first + ne->respawn + 0.5;
		run(w, 2.0);
		int back = raiders(w, code);
		ok = back == 1 && w->nest_lost[gi][0] == ne->count - 1;
		failed += !ok;
		trs_log_printf(out, "%s %.0f h after the first went down: %d raider back up (%d still to make up)\n", ok ? "ok  " : "FAIL",
				ne->respawn / 3600, back, w->nest_lost[gi][0]);
	}

	/* 8. they go home when we leave, and before a rails warp */
	{
		trs_world_remove_vessel(w, gd);
		int away = put(w, &probe, gi, along(b, nest_ground, 1.0, 40000), 1.0);
		trs_world_set_focus(w, away);
		run(w, 2.0);
		int left = raiders(w, code);
		int near2 = put(w, &probe, gi, along(b, nest_ground, 1.0, 10000), 1.0);
		trs_world_set_focus(w, near2);
		run(w, 2.0);
		int again = raiders(w, code);
		trs_event ev;
		trs_world_warp(w, 60.0, -1, &ev);
		int warped = raiders(w, code);
		ok = left == 0 && again > 0 && warped == 0;
		failed += !ok;
		trs_log_printf(out, "%s we leave: %d up; back near: %d up; a rails warp: %d up\n", ok ? "ok  " : "FAIL", left, again, warped);
	}

	/* 9. a drill digging 25 km off (far outside their reach, and their
	 *    waking) is heard: they go up and come for it, fast and slowing to
	 *    fight; quiet, nobody comes */
	{
		trs_craft dr;
		trs_craft_copy(&dr, &probe);
		trs_craft_add(&dr, trs_catalog_find(&cat, "ore_bin"), 0, "", "", v3(-0.6, 0, 0), quat_identity(), -1, 0);
		trs_craft_add(&dr, trs_catalog_find(&cat, "drill"), 0, "", "", v3(0, -0.4, 0.5), quat_identity(), -1, 0);
		/* (batteries for minutes of digging; on legs: a lander that stays
		 * standing when it is let go) */
		for (int k = 0; k < 6; k++)
			trs_craft_add(&dr, trs_catalog_find(&cat, "battery"), 0, "", "", v3(0.6 * cos(k), 0.3, 0.6 * sin(k)), quat_identity(), -1, 0);
		for (int k = 0; k < 4; k++) {
			double a = TRS_PI / 4 + k * TRS_PI / 2;
			int l = trs_craft_add(&dr, trs_catalog_find(&cat, "landing_leg"), 0, "", "", v3(0.5 * cos(a), -0.2, 0.5 * sin(a)),
					quat_axis_angle(v3(0, 1, 0), TRS_PI / 2 - a), -1, 0);
			dr.parts[l].legs_deployed = 1;
		}
		int up_q = -1, heard[2] = { 0, 0 }, after = 0;
		double arrive = -1, fastest = 0;
		char who[64] = "";
		for (int dig = 0; dig < 2; dig++) {
			trs_world *w9 = malloc(sizeof(trs_world));
			trs_world_init(w9, &sys, t0);
			trs_world_set_nest_craft(w9, ne->craft, &raider);
			/* (the dusk crawlers about there would come for it too: calmed,
			 * in this world, so it is the Jaegers alone that come) */
			for (int k = 0; k < w9->sys.bodies[gi].nlife; k++)
				if (w9->sys.bodies[gi].life[k].temper == TRS_TEMPER_HOSTILE)
					w9->sys.bodies[gi].life[k].temper = TRS_TEMPER_CALM;
			int d9 = put(w9, &dr, gi, along(b, nest_ground, 1.0, 25000), 2.0);
			trs_world_set_focus(w9, d9);
			/* (let go and settled on its legs, as a ship set down is) */
			trs_vessel_set_mode(w9, d9, TRS_MODE_PHYSICS);
			w9->vessels[d9].sas = TRS_SAS_UPRIGHT;
			run(w9, 8.0);
			trs_world_set_isru(w9, d9, dig);
			run(w9, 1.0);
			trs_world_step_physics(w9);
			heard[dig] = trs_world_drill_heard(w9, d9, who, sizeof who);
			for (int s = 0; s < 240 * TRS_PHYSICS_HZ && arrive < 0; s++) {
				trs_world_step_physics(w9);
				if (!dig && s == 10 * TRS_PHYSICS_HZ) {
					up_q = raiders(w9, code);
					break;
				}
				w9->nevents = 0;
				trs_v3 at = w9->vessels[d9].mode == TRS_MODE_LANDED ? trs_body_from_fixed(b, w9->t, w9->vessels[d9].fixed_r) : w9->vessels[d9].r;
				for (int i = 0; i < w9->nvessels; i++) {
					const trs_vessel *vs = &w9->vessels[i];
					if (!vs->alive || vs->pilot != code)
						continue;
					after |= vs->pilot_state == 1 && vs->pilot_target.id == d9;
					fastest = fmax(fastest, v3_len(v3_sub(vs->v, v3_cross(trs_body_omega(b), vs->r))));
					if (v3_len(v3_sub(vs->r, at)) < 700 && arrive < 0)
						arrive = s * h;
				}
			}
			trs_world_free(w9);
			free(w9);
		}
		trs_craft_free(&dr);
		ok = b->nests[0].hear > 25000 && up_q == 0 && heard[0] == TRS_HEARD_NONE && (heard[1] == TRS_HEARD_NEST || heard[1] == TRS_HEARD_RAIDERS) && after && arrive > 0 && arrive < 240;
		failed += !ok;
		trs_log_printf(out, "%s a drill digging 25 km from %s (it hears %.0f km): heard at once (%d), they come for it (%s), the first within "
				"700 m %.0f s on, at most %.0f m/s on the way; quiet: heard %d, %d up after 10 s\n", ok ? "ok  " : "FAIL", ne->name, b->nests[0].hear / 1000,
				heard[1], after ? "yes" : "no", arrive, fastest, heard[0], up_q);
	}
	trs_world_free(w);
	free(w);
	trs_craft_free(&raider);
	trs_craft_free(&probe);
	trs_craft_free(&guard);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
