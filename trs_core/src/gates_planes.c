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

/* test-planes: jets, wheels and the runway. A jet's fuel flow (and so its
 * thrust) is its static flow x density / rho_ref x (1 - (v / v_max)^2):
 * checked at rest at sea level, at 5 km and 150 m/s, past its top speed
 * and in space. The stock Gull on the runway: braked it stays put;
 * rolling free it slows by about mu_roll g; braking by mu_brake g; a
 * sideways slide is gripped out; the nose wheel steers; and at full
 * throttle it lifts off well within the flat shelf and climbs. */
#include "trs/gate.h"
#include "trs/json.h"
#include "trs/world.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int trs_gate_load_catalog(trs_log *out, const char *data_dir, trs_part_catalog *cat);

static void run(trs_world *w, double s) {
	for (int k = 0; k < (int)lround(s * TRS_PHYSICS_HZ); k++)
		trs_world_step_physics(w);
}

/* ground velocity along the nose, and across it (m/s) */
static void roll_vel(const trs_world *w, int id, double *along, double *across) {
	const trs_vessel *vs = &w->vessels[id];
	trs_v3 rel = v3_sub(vs->v, v3_cross(trs_body_omega(&w->sys.bodies[vs->body]), vs->r));
	trs_v3 up = v3_norm(vs->r), nose = quat_rotate(vs->q, v3(0, 1, 0));
	nose = v3_norm(v3_sub(nose, v3_scale(up, v3_dot(nose, up))));
	trs_v3 side = v3_cross(up, nose);
	*along = v3_dot(rel, nose), *across = v3_dot(rel, side);
}

static double heading(const trs_vessel *vs) {
	trs_v3 up = v3_norm(vs->r), nose = quat_rotate(vs->q, v3(0, 1, 0));
	trs_v3 e = v3_norm(v3_cross(v3(0, 0, 1), up)), n = v3_cross(up, e);
	return atan2(v3_dot(nose, e), v3_dot(nose, n)) * 180 / TRS_PI;
}

/* sets the vessel moving over the ground along its nose (and across) */
static void push(trs_world *w, int id, double along, double across) {
	trs_vessel *vs = &w->vessels[id];
	trs_v3 up = v3_norm(vs->r), nose = quat_rotate(vs->q, v3(0, 1, 0));
	nose = v3_norm(v3_sub(nose, v3_scale(up, v3_dot(nose, up))));
	trs_v3 side = v3_cross(up, nose);
	vs->v = v3_add(v3_cross(trs_body_omega(&w->sys.bodies[vs->body]), vs->r), v3_add(v3_scale(nose, along), v3_scale(side, across)));
}

trs_gate_status trs_gate_planes(trs_log *out, const char *data_dir) {
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
	snprintf(path, sizeof path, "%s/craft/gull.craft.json", data_dir);
	trs_json *gj = trs_json_load_file(path, err, sizeof err);
	trs_craft gull;
	if (!gj || !trs_craft_from_json(&gull, &cat, gj, err, sizeof err)) {
		trs_log_printf(out, "cannot load the Gull: %s\n", err);
		return TRS_GATE_FAIL;
	}
	trs_json_free(gj);
	int floe = trs_system_find(&sys, "floe");
	const trs_body *F = &sys.bodies[floe];
	int failed = 0;
	int jet = -1;
	for (int i = 0; i < gull.count; i++)
		if (trs_craft_def(&gull, i)->air_breathing)
			jet = i;
	const trs_part_def *jd = trs_craft_def(&gull, jet);
	double mdot0 = trs_engine_mdot(jd), g = F->mu / (F->radius * F->radius);

	/* 1. jet flow at four places: held still in its state for one tick */
	{
		struct { const char *what; double alt, speed; int vacuum; } cases[] = {
			{ "at rest at sea level", 0, 0, 0 }, { "at 5 km, 150 m/s", 5000, 150, 0 },
			{ "at 2 km, 600 m/s (past v_max)", 2000, 600, 0 }, { "in orbit at 150 km", 150000, 0, 1 },
		};
		for (int k = 0; k < 4; k++) {
			trs_world *w = malloc(sizeof(trs_world));
			trs_world_init(w, &sys, 0);
			/* "sea level" sampled 3 km up a 0-height column would not be sea
			 * level: hold the craft at the altitude asked for, over the sea
			 * (above any terrain there: only one tick passes) */
			trs_v3 r = v3(F->radius + cases[k].alt, 0, 0);
			trs_v3 v = cases[k].vacuum ? v3(0, sqrt(F->mu / v3_len(r)), 0) : v3_add(v3_cross(trs_body_omega(F), r), v3(0, cases[k].speed, 0));
			int id = trs_world_add_craft(w, &gull, floe, r, v, quat_identity());
			trs_vessel *vs = &w->vessels[id];
			vs->sas = TRS_SAS_OFF;
			vs->throttle = 1;
			trs_world_stage(w, id, NULL, 0);
			double fuel0 = 0, fuel1 = 0;
			for (int i = 0; i < vs->craft->count; i++)
				fuel0 += vs->craft->parts[i].fuel;
			trs_world_step_physics(w);
			for (int i = 0; i < vs->craft->count; i++)
				fuel1 += vs->craft->parts[i].fuel;
			double flow = (fuel0 - fuel1) / TRS_PHYSICS_DT;
			/* the tick uses the density and airspeed at its start: the
			 * state it was put in */
			double alt = v3_len(r) - F->radius, p, rho;
			trs_atmosphere_at(F, alt, &p, &rho);
			double s = cases[k].vacuum ? 0 : cases[k].speed / jd->jet_v_max;
			double want = cases[k].vacuum ? 0 : mdot0 * rho / jd->jet_rho_ref * fmax(0, 1 - s * s);
			int ok = fabs(flow - want) <= 1e-9 * fmax(1, mdot0);
			failed += !ok;
			trs_log_printf(out, "%s jet %s: %.6f kg/s (want %.6f = %.4f x %.3f x %.3f)\n", ok ? "ok  " : "FAIL", cases[k].what, flow, want, mdot0,
					cases[k].vacuum ? 0.0 : rho / jd->jet_rho_ref, cases[k].vacuum ? 0.0 : fmax(0, 1 - s * s));
			trs_world_free(w);
			free(w);
		}
	}
	/* 2. on the runway */
	trs_world *w = malloc(sizeof(trs_world));
	trs_world_init(w, &sys, 0);
	int id = trs_world_launch_runway(w, &gull, floe, F->site_lat, F->site_lon);
	trs_vessel *vs = &w->vessels[id];
	vs->sas = TRS_SAS_STABILITY;
	run(w, 3);
	double a0, c0;
	roll_vel(w, id, &a0, &c0);
	int wheels_down = vs->contacts;
	{
		int ok = vs->brakes && wheels_down == 3 && fabs(a0) < 0.06 && fabs(c0) < 0.06 && vs->alive; /* the springs still settling */
		failed += !ok;
		trs_log_printf(out, "%s on the runway, brakes on: %d wheels down, %.3f m/s along, %.3f across\n", ok ? "ok  " : "FAIL", wheels_down, a0, c0);
	}
	/* rolling free from 20 m/s, then braking from 20 m/s */
	for (int brake = 0; brake < 2; brake++) {
		vs->brakes = brake;
		push(w, id, 20, 0);
		run(w, 0.5);
		double v1, x1, v2, x2;
		roll_vel(w, id, &v1, &x1);
		run(w, brake ? 1.0 : 3.0);
		roll_vel(w, id, &v2, &x2);
		double decel = (v1 - v2) / (brake ? 1.0 : 3.0);
		/* the wheels carry the weight (wings lift little at 20 m/s); drag
		 * and the tail's lift add a little */
		const trs_part_def *wd = NULL;
		for (int i = 0; i < vs->craft->count; i++)
			if (trs_craft_def(vs->craft, i)->is_wheel)
				wd = trs_craft_def(vs->craft, i);
		double mu = brake ? wd->wheel_mu_brake : wd->wheel_mu_roll;
		int ok = decel > mu * g * 0.9 && decel < mu * g * 1.1 + 0.15;
		failed += !ok;
		trs_log_printf(out, "%s %s from 20 m/s: slows %.3f m/s2 (mu %.2f x g %.2f = %.3f, plus a little drag)\n", ok ? "ok  " : "FAIL",
				brake ? "braking" : "rolling free", decel, mu, g, mu * g);
	}
	/* a sideways slide is gripped out */
	{
		vs->brakes = 0;
		push(w, id, 10, 3);
		run(w, 1.5);
		double a, c;
		roll_vel(w, id, &a, &c);
		int ok = fabs(c) < 0.1 && a > 8.5 && vs->alive;
		failed += !ok;
		trs_log_printf(out, "%s sliding sideways at 3 m/s while rolling at 10: after 1.5 s %.3f m/s across, %.2f along\n", ok ? "ok  " : "FAIL", c, a);
	}
	/* the nose wheel steers: yaw input turns it, none keeps it straight
	 * (slowly enough that the tyres don't slide in the turn) */
	trs_world_free(w);
	trs_world_init(w, &sys, 0);
	id = trs_world_launch_runway(w, &gull, floe, F->site_lat, F->site_lon);
	vs = &w->vessels[id];
	vs->sas = TRS_SAS_STABILITY;
	run(w, 2);
	vs->brakes = 0;
	{
		push(w, id, 5, 0);
		run(w, 1.0);
		double h0 = heading(vs);
		run(w, 2.0);
		double straight = heading(vs) - h0;
		trs_v3 up_local = quat_rotate(quat_conj(vs->q), v3_norm(vs->r)); /* the yaw axis in the craft's frame */
		double hb = heading(vs);
		push(w, id, 5, 0);
		vs->input = v3(up_local.x, up_local.z, up_local.y); /* a turn about the local vertical */
		run(w, 2.0);
		double turned = heading(vs) - hb;
		vs->input = v3(0, 0, 0);
		/* a wheel steered by d on a wheelbase L turns the craft at v tan(d) / L */
		double nose_y = -1e9, main_y = 0;
		int mains = 0;
		const trs_part_def *nd = NULL;
		for (int i = 0; i < vs->craft->count; i++) {
			const trs_part_def *d = trs_craft_def(vs->craft, i);
			if (d->is_wheel && d->wheel_steer > 0)
				nose_y = vs->craft->parts[i].pos.y, nd = d;
			else if (d->is_wheel)
				main_y += vs->craft->parts[i].pos.y, mains++;
		}
		double L = nose_y - main_y / mains;
		double rolled = 0, a, c;
		roll_vel(w, id, &a, &c);
		(void)rolled;
		double want = 0.5 * (5 + a) * tan(nd->wheel_steer) / L * 2 * 180 / TRS_PI; /* at the mean speed over the turn */
		int ok = fabs(straight) < 1.0 && fabs(fabs(turned) / want - 1) < 0.15;
		failed += !ok;
		trs_log_printf(out, "%s nose-wheel steering at 5 m/s: straight %.2f deg in 2 s; turning %.1f deg in 2 s (v tan(%.0f deg) / %.2f m wheelbase: %.1f)\n",
				ok ? "ok  " : "FAIL", straight, turned, nd->wheel_steer * 180 / TRS_PI, L, want);
	}
	/* full throttle from a fresh start on the runway, rotate at 95 m/s, and up */
	trs_world_free(w);
	trs_world_init(w, &sys, 0);
	id = trs_world_launch_runway(w, &gull, floe, F->site_lat, F->site_lon);
	vs = &w->vessels[id];
	vs->sas = TRS_SAS_STABILITY;
	run(w, 2);
	{
		vs->brakes = 0;
		vs->throttle = 1;
		trs_world_stage(w, id, NULL, 0);
		trs_v3 start = trs_body_to_fixed(F, w->t, vs->r);
		double lift_m = -1, lift_v = 0, alt0 = v3_len(vs->r);
		for (int k = 0; k < 50 * 40; k++) {
			double a, c;
			roll_vel(w, id, &a, &c);
			vs->input = v3(a > 95 && v3_len(vs->r) - alt0 < 40 ? 1 : 0, 0, 0);
			trs_world_step_physics(w);
			if (lift_m < 0 && vs->contacts == 0 && v3_len(vs->r) - alt0 > 2) {
				lift_m = v3_len(v3_sub(trs_body_to_fixed(F, w->t, vs->r), start));
				lift_v = a;
			}
			if (v3_len(vs->r) - alt0 > 300)
				break;
		}
		double climbed = v3_len(vs->r) - alt0;
		int ok = vs->alive && lift_m > 0 && lift_m < F->terrain.site_radius - 400.0 && climbed > 300;
		failed += !ok;
		trs_log_printf(out, "%s takeoff: wheels up after %.0f m at %.1f m/s, then %.0f m up (the shelf is flat for %.0f m past the runway start)\n",
				ok ? "ok  " : "FAIL", lift_m, lift_v, climbed, F->terrain.site_radius - 400.0);
	}
	trs_world_free(w);
	free(w);
	trs_craft_free(&gull);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}

/* test-actions: action groups. Groups set on parts survive the craft file
 * and the world save; group 1 lights a liquid engine and a second press
 * puts it out (thrust there and gone); group 2 arms the parachute; group 3
 * puts the legs down and up; an empty group does nothing. */
static int stack_on(trs_craft *c, const trs_part_catalog *cat, int parent, const char *id) {
	int di = trs_catalog_find(cat, id);
	const trs_attach_node *pb = trs_part_node(trs_craft_def(c, parent), "bottom");
	const trs_attach_node *ct = trs_part_node(&cat->defs[di], "top");
	return trs_craft_add(c, di, parent, "top", "bottom", v3_sub(v3_add(c->parts[parent].pos, pb->pos), ct->pos), quat_identity(), 0, 0);
}

trs_gate_status trs_gate_actions(trs_log *out, const char *data_dir) {
	trs_part_catalog cat;
	trs_system sys;
	char path[1024], err[256];
	if (!trs_gate_load_catalog(out, data_dir, &cat))
		return TRS_GATE_FAIL;
	snprintf(path, sizeof path, "%s/bodies/austra_system.json", data_dir);
	if (!trs_system_load(&sys, path, err, sizeof err))
		return TRS_GATE_FAIL;
	int floe = trs_system_find(&sys, "floe");
	const trs_body *F = &sys.bodies[floe];
	int failed = 0;
	trs_craft c;
	trs_craft_init(&c, &cat);
	int pod = trs_craft_add(&c, trs_catalog_find(&cat, "pod_1"), -1, "", "", v3(0, 0, 0), quat_identity(), -1, 0);
	int tank = stack_on(&c, &cat, pod, "tank_m");
	int eng = stack_on(&c, &cat, tank, "engine_small");
	int chute = trs_craft_add(&c, trs_catalog_find(&cat, "parachute"), pod, "bottom", "top", v3(0, 0.675, 0), quat_identity(), 0, 0);
	int leg = trs_craft_add(&c, trs_catalog_find(&cat, "landing_leg"), tank, "", "", v3(0, 0, 0.625), quat_identity(), -1, 0);
	c.parts[eng].groups = 1 << 0;
	c.parts[chute].groups = 1 << 1;
	c.parts[leg].groups = 1 << 2;
	/* through the craft file and back */
	trs_json *j = trs_craft_to_json(&c);
	trs_craft back;
	int read = trs_craft_from_json(&back, &cat, j, err, sizeof err);
	trs_json_free(j);
	{
		int ok = read && back.parts[eng].groups == 1 && back.parts[chute].groups == 2 && back.parts[leg].groups == 4 && back.parts[pod].groups == 0;
		failed += !ok;
		trs_log_printf(out, "%s groups through the craft file: engine %d, chute %d, leg %d, pod %d\n", ok ? "ok  " : "FAIL",
				back.parts[eng].groups, back.parts[chute].groups, back.parts[leg].groups, back.parts[pod].groups);
	}
	trs_world *w = malloc(sizeof(trs_world));
	trs_world_init(w, &sys, 0);
	trs_v3 r = v3(F->radius + 100000, 0, 0);
	int id = trs_world_add_craft(w, &back, floe, r, v3(0, sqrt(F->mu / v3_len(r)), 0), quat_identity());
	trs_vessel *vs = &w->vessels[id];
	vs->throttle = 1;
	vs->sas = TRS_SAS_OFF;
	double fuel0 = vs->craft->parts[tank].fuel;
	int n1 = trs_world_action_group(w, id, 1);
	trs_world_step_physics(w);
	double burned = fuel0 - vs->craft->parts[tank].fuel;
	int n1b = trs_world_action_group(w, id, 1);
	double fuel1 = vs->craft->parts[tank].fuel;
	trs_world_step_physics(w);
	double burned_after = fuel1 - vs->craft->parts[tank].fuel;
	{
		int ok = n1 == 1 && n1b == 1 && burned > 0 && burned_after == 0;
		failed += !ok;
		trs_log_printf(out, "%s group 1: engine lit (%.4f kg burned in a tick), pressed again: out (%.4f kg)\n", ok ? "ok  " : "FAIL", burned, burned_after);
	}
	{
		int n2 = trs_world_action_group(w, id, 2);
		int armed = vs->craft->parts[chute].chute == TRS_CHUTE_ARMED;
		int n3 = trs_world_action_group(w, id, 3), down = vs->craft->parts[leg].legs_deployed;
		int n3b = trs_world_action_group(w, id, 3), up = !vs->craft->parts[leg].legs_deployed;
		int n9 = trs_world_action_group(w, id, 9);
		int ok = n2 == 1 && armed && n3 == 1 && down && n3b == 1 && up && n9 == 0;
		failed += !ok;
		trs_log_printf(out, "%s group 2 arms the chute (%s); group 3 legs down (%s) and up (%s); group 9, empty: %d things\n", ok ? "ok  " : "FAIL",
				armed ? "armed" : "no", down ? "yes" : "no", up ? "yes" : "no", n9);
	}
	/* through the world save */
	{
		trs_json *wj = trs_world_to_json(w);
		trs_world *w2 = malloc(sizeof(trs_world));
		trs_world_init(w2, &sys, 0);
		int loaded = trs_world_from_json(w2, &cat, wj, err, sizeof err);
		int ok = loaded && w2->vessels[id].craft->parts[eng].groups == 1 && w2->vessels[id].craft->parts[leg].groups == 4;
		failed += !ok;
		trs_log_printf(out, "%s groups through the world save: %s\n", ok ? "ok  " : "FAIL", ok ? "kept" : err);
		trs_json_free(wj);
		trs_world_free(w2);
		free(w2);
	}
	trs_world_free(w);
	free(w);
	trs_craft_free(&back);
	trs_craft_free(&c);
	trs_catalog_free(&cat);
	return failed ? TRS_GATE_FAIL : TRS_GATE_PASS;
}
